/*
 * Minimal sunxi-vin capture test.
 * cap <w> <h> <fourcc> <nframes> <exp_lines> <gain_x100> <outprefix> [ctrl_id=val ...]
 * Sets exposure/gain after STREAMON (sensor soft-resets on stream start),
 * prints per-frame seq/timestamp/bytesused, saves the last frame and a
 * 4x-downscaled 8-bit PGM of it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <linux/videodev2.h>

#ifndef V4L2_MODE_VIDEO
#define V4L2_MODE_VIDEO 0x0002
#endif
#define NBUF 4
static struct { void *p; size_t len; } bufs[NBUF];

static int set_ctrl(int fd, unsigned id, int val)
{
	struct v4l2_control c = { .id = id, .value = val };
	int r = ioctl(fd, VIDIOC_S_CTRL, &c);
	printf("S_CTRL 0x%x=%d -> %d%s\n", id, val, r, r ? strerror(errno) : "");
	return r;
}

#define CCI "/sys/devices/ar0234_mipi/"
static void sysfs_put(const char *f, const char *s)
{
	FILE *fp = fopen(f, "w");
	if (!fp) { perror(f); return; }
	fputs(s, fp);
	fclose(fp);
}

static void cci_regs(const char *list, int rd)
{
	char buf[1024], *tok, *save;
	if (!list) return;
	snprintf(buf, sizeof(buf), "%s", list);
	sysfs_put(CCI "read_flag", rd ? "1" : "0");
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		unsigned reg, val = 0;
		char cmd[16];
		if (rd) {
			sscanf(tok, "%x", &reg);
			snprintf(cmd, sizeof(cmd), "%04x0000", reg);
			sysfs_put(CCI "cci_client", cmd);
			FILE *fp = fopen(CCI "read_value", "r");
			if (fp) { fscanf(fp, "%x", &val); fclose(fp); }
			printf("R 0x%04x = 0x%04x\n", reg, val);
		} else {
			sscanf(tok, "%x=%x", &reg, &val);
			snprintf(cmd, sizeof(cmd), "%04x%04x", reg, val);
			sysfs_put(CCI "cci_client", cmd);
			printf("W 0x%04x = 0x%04x\n", reg, val);
		}
	}
	sysfs_put(CCI "read_flag", "0");
}

int main(int argc, char **argv)
{
	if (argc < 8) {
		fprintf(stderr, "usage: %s w h fourcc n exp_lines gain_x100 outprefix [id=val..]\n", argv[0]);
		return 1;
	}
	int w = atoi(argv[1]), h = atoi(argv[2]);
	const char *fc = argv[3];
	int n = atoi(argv[4]), exp_lines = atoi(argv[5]), gain = atoi(argv[6]);
	const char *out = argv[7];
	int fd = open("/dev/video0", O_RDWR | O_NONBLOCK);
	if (fd < 0) { perror("open"); return 1; }

	struct v4l2_input inp = { .index = 0 };
	if (ioctl(fd, VIDIOC_S_INPUT, &inp)) { perror("S_INPUT"); return 1; }

	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = 120;
	parm.parm.capture.capturemode = V4L2_MODE_VIDEO;
	if (ioctl(fd, VIDIOC_S_PARM, &parm)) perror("S_PARM");

	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = v4l2_fourcc(fc[0], fc[1], fc[2], fc[3]);
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (ioctl(fd, VIDIOC_S_FMT, &f)) { perror("S_FMT"); return 1; }
	ioctl(fd, VIDIOC_G_FMT, &f);
	printf("fmt %dx%d %.4s planes=%d sizeimage=%u bpl=%u\n", f.fmt.pix_mp.width,
	       f.fmt.pix_mp.height, (char *)&f.fmt.pix_mp.pixelformat,
	       f.fmt.pix_mp.num_planes, f.fmt.pix_mp.plane_fmt[0].sizeimage,
	       f.fmt.pix_mp.plane_fmt[0].bytesperline);
	w = f.fmt.pix_mp.width;
	h = f.fmt.pix_mp.height;
	int np = f.fmt.pix_mp.num_planes;

	struct v4l2_requestbuffers rb = { .count = NBUF,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
	if (ioctl(fd, VIDIOC_REQBUFS, &rb)) { perror("REQBUFS"); return 1; }
	for (unsigned i = 0; i < rb.count; i++) {
		struct v4l2_plane pl[VIDEO_MAX_PLANES];
		struct v4l2_buffer b = { .index = i, .type = rb.type, .memory = rb.memory,
			.m.planes = pl, .length = np };
		if (ioctl(fd, VIDIOC_QUERYBUF, &b)) { perror("QUERYBUF"); return 1; }
		bufs[i].len = pl[0].length;
		bufs[i].p = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
				 fd, pl[0].m.mem_offset);
		if (bufs[i].p == MAP_FAILED) { perror("mmap"); return 1; }
		if (ioctl(fd, VIDIOC_QBUF, &b)) { perror("QBUF"); return 1; }
	}
	enum v4l2_buf_type t = rb.type;
	if (ioctl(fd, VIDIOC_STREAMON, &t)) { perror("STREAMON"); return 1; }

	if (exp_lines > 0)
		set_ctrl(fd, V4L2_CID_EXPOSURE, exp_lines << 4);
	if (gain > 0)
		set_ctrl(fd, V4L2_CID_GAIN, gain);
	for (int a = 8; a < argc; a++) {
		unsigned id; int v;
		if (sscanf(argv[a], "%i=%i", &id, &v) == 2)
			set_ctrl(fd, id, v);
	}

	/* WREG="3070=0002,..." writes, RREG="3012,3060" reads via cci sysfs */
	cci_regs(getenv("WREG"), 0);
	cci_regs(getenv("RREG"), 1);

	/*
	 * AE=<target 10-bit mean above black> runs a simple exposure loop,
	 * SAVEALL=<file> appends every frame after WARMUP=<n> frames.
	 */
	int ae_target = getenv("AE") ? atoi(getenv("AE")) : 0;
	int warmup = getenv("WARMUP") ? atoi(getenv("WARMUP")) : 0;
	int ae_max_lines = getenv("AEMAX") ? atoi(getenv("AEMAX")) : 4800;
	FILE *saveall = getenv("SAVEALL") ? fopen(getenv("SAVEALL"), "wb") : NULL;
	double ae_exp = exp_lines > 0 ? exp_lines : 100, ae_gain = gain > 0 ? gain / 1600.0 : 1.0;
	int saved = 0;

	int got = 0, last_idx = -1, last_used = 0;
	double t0 = 0, t1 = 0;
	while (got < n) {
		fd_set fs; FD_ZERO(&fs); FD_SET(fd, &fs);
		struct timeval tv = { 2, 0 };
		int r = select(fd + 1, &fs, NULL, NULL, &tv);
		if (r <= 0) { printf("select timeout/err r=%d after %d frames\n", r, got); break; }
		struct v4l2_plane pl[VIDEO_MAX_PLANES];
		struct v4l2_buffer b = { .type = t, .memory = V4L2_MEMORY_MMAP,
			.m.planes = pl, .length = np };
		if (ioctl(fd, VIDIOC_DQBUF, &b)) { if (errno == EAGAIN) continue; perror("DQBUF"); break; }
		double ts = b.timestamp.tv_sec + b.timestamp.tv_usec / 1e6;
		if (got == 0) t0 = ts;
		t1 = ts;
		if (got < 5 || got == n - 1)
			printf("frame %d seq=%u used=%u flags=0x%x ts=%.6f\n", got, b.sequence,
			       pl[0].bytesused, b.flags, ts);
		got++;
		last_idx = b.index;
		last_used = pl[0].bytesused;
		if (saveall && got > warmup) {
			fwrite(bufs[b.index].p, 1, (size_t)w * h * 2, saveall);
			saved++;
		}
		if (ae_target > 0 && got % 3 == 0) {
			unsigned short *px = bufs[b.index].p;
			double sum = 0;
			long cnt = 0;
			for (size_t o = 0; o < (size_t)w * h; o += 97) {
				sum += px[o];
				cnt++;
			}
			double mean = sum / cnt - 42;
			if (mean < 1)
				mean = 1;
			double total = ae_exp * ae_gain * ae_target / mean;
			double ratio = total / (ae_exp * ae_gain);
			if (ratio > 2) total = ae_exp * ae_gain * 2;	/* damp */
			if (ratio < 0.5) total = ae_exp * ae_gain * 0.5;
			ae_exp = total > ae_max_lines ? ae_max_lines : (total < 2 ? 2 : total);
			ae_gain = total / ae_exp;
			if (ae_gain < 1) ae_gain = 1;
			if (ae_gain > 32) ae_gain = 32;
			set_ctrl(fd, V4L2_CID_EXPOSURE, (int)ae_exp << 4);
			set_ctrl(fd, V4L2_CID_GAIN, (int)(ae_gain * 1600));
			if (got % 30 == 0)
				printf("AE frame %d mean=%.1f exp=%.0f lines gain=%.2fx\n", got, mean, ae_exp, ae_gain);
		}
		if (got < n)
			ioctl(fd, VIDIOC_QBUF, &b);
	}
	if (got > 1)
		printf("got %d frames, avg fps %.2f\n", got, (got - 1) / (t1 - t0));
	if (saveall) {
		fclose(saveall);
		printf("SAVEALL: %d frames\n", saved);
	}

	if (last_idx >= 0) {
		char path[256];
		unsigned char *p = bufs[last_idx].p;
		snprintf(path, sizeof(path), "%s.raw", out);
		FILE *fp = fopen(path, "wb");
		fwrite(p, 1, last_used ? last_used : bufs[last_idx].len, fp);
		fclose(fp);
		/* 8-bit stride guess: bytesused / (w*h) */
		size_t len = last_used ? last_used : bufs[last_idx].len;
		int bpp = len / ((size_t)w * h);
		if (bpp < 1) bpp = 1;
		if (getenv("BPP")) bpp = atoi(getenv("BPP"));
		snprintf(path, sizeof(path), "%s.pgm", out);
		fp = fopen(path, "wb");
		int ow = w / 4, oh = h / 4;
		fprintf(fp, "P5\n%d %d\n255\n", ow, oh);
		for (int y = 0; y < oh; y++)
			for (int x = 0; x < ow; x++) {
				unsigned s = 0;
				for (int dy = 0; dy < 4; dy++)
					for (int dx = 0; dx < 4; dx++) {
						size_t o = ((size_t)(y * 4 + dy) * w + x * 4 + dx) * bpp;
						s += bpp == 2 ? ((p[o] | p[o + 1] << 8) >> 2) : p[o];
					}
				fputc(s / 16 > 255 ? 255 : s / 16, fp);
			}
		fclose(fp);
		printf("saved %s.raw (%zu bytes, bpp=%d) and %s.pgm %dx%d\n", out, len, bpp, out, ow, oh);
	}
	ioctl(fd, VIDIOC_STREAMOFF, &t);
	close(fd);
	return got == n ? 0 : 2;
}

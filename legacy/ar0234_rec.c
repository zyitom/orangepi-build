/*
 * ar0234_rec: record H.264 from the AR0234 using the A733 hardware only
 *   sunxi-vin ISP + scaler (NV12) -> libisp 3A (AE/AWB) -> cedar VE H.264
 *
 * ar0234_rec [-w 1920] [-h 1080] [-f 30] [-n 300] [-b 8000000] [-o out.h264] [-I]
 *   -I  do not start the libisp 3A helper (raw ISP defaults)
 *   -T  encoder self test: synthetic NV12 frames, no camera
 *   -C  copy frames into encoder buffers instead of zero-copy DMA-BUF import
 *
 * 3A runs in the ar0234_3a helper process (see there why), found next to
 * this binary or via $AR0234_3A.
 *
 * build: gcc -O2 -o ar0234_rec ar0234_rec.c -lvencoder -lvenc_base -lvenc_codec \
 *        -lVE -lMemAdapter -lcdc_base
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include <memoryAdapter.h>
#include <veAdapter.h>
#include <veInterface.h>
#include <vencoder.h>

#ifndef V4L2_MODE_VIDEO
#define V4L2_MODE_VIDEO 0x0002
#endif
#define NBUF 4
#define ALIGN16(x) (((x) + 15) & ~15)

static volatile int stop;
static void on_sig(int s) { (void)s; stop = 1; }

static double now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
	int w = 1920, h = 1080, fps = 30, frames = 300, bitrate = 8000000, use_isp = 1, selftest = 0;
	int zerocopy = 1, dmafd[NBUF];
	struct user_iommu_param iova[NBUF];
	const char *out = "ar0234.h264";
	struct { void *p; size_t len; } bufs[NBUF];
	int c;

	while ((c = getopt(argc, argv, "w:h:f:n:b:o:ITC")) != -1) {
		switch (c) {
		case 'w': w = atoi(optarg); break;
		case 'h': h = atoi(optarg); break;
		case 'f': fps = atoi(optarg); break;
		case 'n': frames = atoi(optarg); break;
		case 'b': bitrate = atoi(optarg); break;
		case 'o': out = optarg; break;
		case 'I': use_isp = 0; break;
		case 'T': selftest = 1; use_isp = 0; zerocopy = 0; break;
		case 'C': zerocopy = 0; break;
		default: fprintf(stderr, "see source header for usage\n"); return 1;
		}
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	pid_t isp_pid = -1;

	/* ---- capture ---- */
	int fd = -1;
	struct v4l2_requestbuffers rb = { .count = NBUF,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
	enum v4l2_buf_type t = rb.type;
	if (selftest)
		goto encoder;
	fd = open("/dev/video0", O_RDWR | O_NONBLOCK);
	if (fd < 0) { perror("open /dev/video0"); return 1; }
	struct v4l2_input inp = { .index = 0 };
	if (ioctl(fd, VIDIOC_S_INPUT, &inp)) { perror("S_INPUT"); return 1; }
	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	parm.parm.capture.capturemode = V4L2_MODE_VIDEO;
	if (ioctl(fd, VIDIOC_S_PARM, &parm)) perror("S_PARM");
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (ioctl(fd, VIDIOC_S_FMT, &f)) { perror("S_FMT"); return 1; }
	ioctl(fd, VIDIOC_G_FMT, &f);
	w = f.fmt.pix_mp.width;
	h = f.fmt.pix_mp.height;
	printf("capture %dx%d NV12 @%d fps, sizeimage %u\n", w, h, fps, f.fmt.pix_mp.plane_fmt[0].sizeimage);

	if (ioctl(fd, VIDIOC_REQBUFS, &rb)) { perror("REQBUFS"); return 1; }
	for (unsigned i = 0; i < rb.count; i++) {
		struct v4l2_plane pl[VIDEO_MAX_PLANES];
		struct v4l2_buffer b = { .index = i, .type = rb.type, .memory = rb.memory,
			.m.planes = pl, .length = 1 };
		if (ioctl(fd, VIDIOC_QUERYBUF, &b)) { perror("QUERYBUF"); return 1; }
		bufs[i].len = pl[0].length;
		bufs[i].p = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				 pl[0].m.mem_offset);
		if (bufs[i].p == MAP_FAILED) { perror("mmap"); return 1; }
		struct v4l2_exportbuffer eb = { .type = rb.type, .index = i, .plane = 0, .flags = O_CLOEXEC };
		dmafd[i] = ioctl(fd, VIDIOC_EXPBUF, &eb) ? -1 : eb.fd;
		if (dmafd[i] < 0)
			zerocopy = 0;
		if (ioctl(fd, VIDIOC_QBUF, &b)) { perror("QBUF"); return 1; }
	}

	/* ---- cedar VE H.264 ---- */
encoder:;
	VencBaseConfig base;
	memset(&base, 0, sizeof(base));
	base.memops = MemAdapterGetOpsS();
	if (!base.memops || CdcMemOpen(base.memops) < 0) { fprintf(stderr, "memops\n"); return 1; }
	/* like libOmxVenc: VideoEncInit() creates and owns the VE instance */
	base.nInputWidth = w;
	base.nInputHeight = h;
	base.nStride = w;
	base.nDstWidth = w;
	base.nDstHeight = h;
	base.eInputFormat = VENC_PIXEL_YUV420SP;

	VideoEncoder *enc = VideoEncCreate(VENC_CODEC_H264);
	if (!enc) { fprintf(stderr, "VideoEncCreate failed\n"); return 1; }
	VencH264Param hp;
	memset(&hp, 0, sizeof(hp));
	hp.sProfileLevel.nProfile = VENC_H264ProfileHigh;
	hp.sProfileLevel.nLevel = VENC_H264Level51;
	hp.bEntropyCodingCABAC = 1;
	hp.sQPRange.nMinqp = 10;
	hp.sQPRange.nMaxqp = 45;
	hp.nFramerate = fps;
	hp.nSrcFramerate = fps;
	hp.nBitrate = bitrate;
	hp.nMaxKeyInterval = fps;
	hp.nCodingMode = VENC_FRAME_CODING;
	VideoEncSetParameter(enc, VENC_IndexParamH264Param, &hp);
	if (VideoEncInit(enc, &base)) { fprintf(stderr, "VideoEncInit failed\n"); return 1; }

	FILE *fp = fopen(out, "wb");
	if (!fp) { perror(out); return 1; }
	VencHeaderData hdr;
	if (!VideoEncGetParameter(enc, VENC_IndexParamH264SPSPPS, &hdr))
		fwrite(hdr.pBuffer, 1, hdr.nLength, fp);

	/*
	 * VE works on 16x16 macroblocks and reads whole aligned planes: a 1080
	 * line buffer makes it fault past the end (IOMMU "invalid address").
	 */
	int aw = ALIGN16(w), ah = ALIGN16(h);
	if (zerocopy) {
		/* vin allocates 16-aligned NV12 buffers, so VE may read them whole */
		for (int i = 0; i < NBUF; i++) {
			iova[i].fd = dmafd[i];
			iova[i].iommu_addr = 0;
			VideoEncoderGetVeIommuAddr(enc, &iova[i]);
			if (!iova[i].iommu_addr) { fprintf(stderr, "VE iommu map failed, fall back to copy\n"); zerocopy = 0; break; }
		}
	}
	printf("input path: %s\n", zerocopy ? "zero-copy DMA-BUF (ISP buffer -> VE)" : "memcpy");
	VencAllocateBufferParam ap = { .nBufferNum = NBUF, .nSizeY = aw * ah, .nSizeC = aw * ah / 2 };
	if (!zerocopy && AllocInputBuffer(enc, &ap)) { fprintf(stderr, "AllocInputBuffer failed\n"); return 1; }

	/* ---- libisp 3A in a helper process ---- */
	if (use_isp) {
		char helper[512];
		int pfd[2];
		const char *env = getenv("AR0234_3A");

		if (env) {
			snprintf(helper, sizeof(helper), "%s", env);
		} else {
			ssize_t n = readlink("/proc/self/exe", helper, sizeof(helper) - 16);
			char *slash;

			helper[n > 0 ? n : 0] = 0;
			slash = strrchr(helper, '/');
			strcpy(slash ? slash + 1 : helper, "ar0234_3a");
		}
		if (pipe(pfd)) { perror("pipe"); return 1; }
		char sw[12], sh[12], sf[12];
		snprintf(sw, sizeof(sw), "%d", w);
		snprintf(sh, sizeof(sh), "%d", h);
		snprintf(sf, sizeof(sf), "%d", fps);
		isp_pid = fork();
		if (isp_pid == 0) {
			dup2(pfd[1], 1);
			close(pfd[0]);
			execl(helper, helper, "0", sw, sh, sf, (char *)NULL);
			perror(helper);
			_exit(127);
		}
		close(pfd[1]);
		char line[64] = "";
		FILE *pf = fdopen(pfd[0], "r");
		while (fgets(line, sizeof(line), pf) && strncmp(line, "ready", 5))
			;
		if (strncmp(line, "ready", 5)) {
			fprintf(stderr, "3A helper %s did not start\n", helper);
			return 1;
		}
		printf("libisp 3A helper running (pid %d, %s", (int)isp_pid, line + 6);
	}

	if (!selftest && ioctl(fd, VIDIOC_STREAMON, &t)) { perror("STREAMON"); return 1; }
	unsigned char *synth = selftest ? malloc(w * h * 3 / 2) : NULL;

	int got = 0;
	long long bytes = 0;
	double t0 = now(), enc_time = 0;
	while (got < frames && !stop) {
		struct v4l2_plane pl[VIDEO_MAX_PLANES];
		struct v4l2_buffer b = { .type = t, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1 };
		unsigned char *src;
		if (selftest) {
			for (int y = 0; y < h; y++)
				memset(synth + y * w, (y + got * 4) & 0xff, w);
			memset(synth + w * h, 128 + (got % 64), w * h / 2);
			src = synth;
			gettimeofday(&b.timestamp, NULL);
		} else {
			fd_set fs;
			FD_ZERO(&fs);
			FD_SET(fd, &fs);
			struct timeval tv = { 2, 0 };
			if (select(fd + 1, &fs, NULL, NULL, &tv) <= 0) { fprintf(stderr, "capture timeout\n"); break; }
			if (ioctl(fd, VIDIOC_DQBUF, &b)) { if (errno == EAGAIN) continue; perror("DQBUF"); break; }
			src = bufs[b.index].p;
		}

		double te = now();
		VencInputBuffer in;
		memset(&in, 0, sizeof(in));
		if (zerocopy) {
			in.nID = b.index;
			in.nPts = (long long)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
			in.pAddrVirY = bufs[b.index].p;
			in.pAddrVirC = (unsigned char *)bufs[b.index].p + w * h;
			in.pAddrPhyY = (unsigned char *)(unsigned long)iova[b.index].iommu_addr;
			in.pAddrPhyC = (unsigned char *)(unsigned long)(iova[b.index].iommu_addr + w * h);
			/*
			 * No share fd: with one VE re-derives the chroma address from
			 * the 16-aligned height (1088) and the bottom rows turn green,
			 * vin puts UV right after the w*h luma plane.
			 */
			in.nShareBufFd = -1;
			AddOneInputBuffer(enc, &in);
			VideoEncodeOneFrame(enc);
			AlreadyUsedInputBuffer(enc, &in);
			VencOutputBuffer ob;
			memset(&ob, 0, sizeof(ob));
			while (GetOneBitstreamFrame(enc, &ob) == 0) {
				fwrite(ob.pData0, 1, ob.nSize0, fp);
				if (ob.nSize1)
					fwrite(ob.pData1, 1, ob.nSize1, fp);
				bytes += ob.nSize0 + ob.nSize1;
				FreeOneBitStreamFrame(enc, &ob);
			}
		} else if (GetOneAllocInputBuffer(enc, &in) == 0) {
			/* copy the w x h NV12 frame, replicate edges into the aligned padding */
			for (int y = 0; y < ah; y++) {
				unsigned char *d = in.pAddrVirY + y * aw;

				memcpy(d, src + (y < h ? y : h - 1) * w, w);
				if (aw > w)
					memset(d + w, d[w - 1], aw - w);
			}
			for (int y = 0; y < ah / 2; y++) {
				unsigned char *d = in.pAddrVirC + y * aw;

				memcpy(d, src + w * h + (y < h / 2 ? y : h / 2 - 1) * w, w);
				for (int x = w; x < aw; x += 2) {
					d[x] = d[w - 2];
					d[x + 1] = d[w - 1];
				}
			}
			in.nPts = (long long)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
			FlushCacheAllocInputBuffer(enc, &in);
			AddOneInputBuffer(enc, &in);
			VideoEncodeOneFrame(enc);
			AlreadyUsedInputBuffer(enc, &in);
			ReturnOneAllocInputBuffer(enc, &in);

			VencOutputBuffer ob;
			memset(&ob, 0, sizeof(ob));
			while (GetOneBitstreamFrame(enc, &ob) == 0) {
				fwrite(ob.pData0, 1, ob.nSize0, fp);
				if (ob.nSize1)
					fwrite(ob.pData1, 1, ob.nSize1, fp);
				bytes += ob.nSize0 + ob.nSize1;
				FreeOneBitStreamFrame(enc, &ob);
			}
		}
		enc_time += now() - te;
		if (!selftest)
			ioctl(fd, VIDIOC_QBUF, &b);
		if (++got % (fps * 2) == 0)
			printf("%d frames, %.2f fps, encode %.1f ms/frame, %.2f Mbit/s\n", got,
			       got / (now() - t0), enc_time * 1000 / got, bytes * 8.0 / (now() - t0) / 1e6);
	}
	double el = now() - t0;
	printf("done: %d frames in %.2fs (%.2f fps), %lld bytes, encode %.1f ms/frame -> %s\n",
	       got, el, got / el, bytes, got ? enc_time * 1000 / got : 0, out);

	if (!selftest)
		ioctl(fd, VIDIOC_STREAMOFF, &t);
	fclose(fp);
	if (zerocopy)
		for (int i = 0; i < NBUF; i++)
			VideoEncoderFreeVeIommuAddr(enc, &iova[i]);
	else
		ReleaseAllocInputBuffer(enc);
	VideoEncUnInit(enc);
	VideoEncDestroy(enc);
	CdcMemClose(base.memops);
	if (isp_pid > 0) {
		kill(isp_pid, SIGTERM);
		waitpid(isp_pid, NULL, 0);
	}
	if (fd >= 0)
		close(fd);
	return got == frames ? 0 : 2;
}

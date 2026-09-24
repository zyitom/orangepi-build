// SPDX-License-Identifier: GPL-2.0-or-later
//
// vfr - capture for N seconds from a sunxi-vin node and report frame rate,
// inter-frame gaps and frames per second. Used to characterise the
// AR0234 / ISP602 frame-loss problem independently of the kernel's own
// (noisy) logging.
//
//   vfr [-d /dev/video0] [-w 1920] [-h 1200] [-f NV12] [-t 60] [-b 4]
//       [-p num/den] [-s 0] [-q]
//
// -p requests a frame rate through VIDIOC_S_PARM (num/den, e.g. 1/120).
// -s sends VIDIOC_S_INPUT with that index (patches/0003 makes the driver do
//    it automatically, so failure is not fatal).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#define MAX_BUF 16

struct buf {
	void *start;
	size_t length;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

static uint32_t parse_fourcc(const char *s)
{
	char c[4] = { ' ', ' ', ' ', ' ' };
	size_t n = strlen(s);

	if (n == 0 || n > 4)
		return 0;
	memcpy(c, s, n);
	return v4l2_fourcc(c[0], c[1], c[2], c[3]);
}

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0";
	unsigned int w = 1920, h = 1200, nbuf = 4, seconds = 60, input = 0;
	uint32_t fourcc = v4l2_fourcc('N', 'V', '1', '2');
	unsigned int pnum = 0, pden = 0;
	int quiet = 0;
	int c;

	while ((c = getopt(argc, argv, "d:w:h:f:t:b:p:s:q")) != -1) {
		switch (c) {
		case 'd': dev = optarg; break;
		case 'w': w = strtoul(optarg, NULL, 0); break;
		case 'h': h = strtoul(optarg, NULL, 0); break;
		case 'f': fourcc = parse_fourcc(optarg); break;
		case 't': seconds = strtoul(optarg, NULL, 0); break;
		case 'b': nbuf = strtoul(optarg, NULL, 0); break;
		case 's': input = strtoul(optarg, NULL, 0); break;
		case 'q': quiet = 1; break;
		case 'p':
			if (sscanf(optarg, "%u/%u", &pnum, &pden) != 2) {
				fprintf(stderr, "bad -p %s (want num/den)\n", optarg);
				return 2;
			}
			break;
		default:
			fprintf(stderr, "usage: %s [-d dev] [-w w] [-h h] [-f NV12] [-t sec]"
					" [-b n] [-s input] [-p num/den] [-q]\n", argv[0]);
			return 2;
		}
	}
	if (nbuf < 2)
		nbuf = 2;
	if (nbuf > MAX_BUF)
		nbuf = MAX_BUF;

	struct buf bufs[MAX_BUF];
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
	int fd = open(dev, O_RDWR | O_NONBLOCK);

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}

	struct v4l2_input in = { .index = input };

	if (xioctl(fd, VIDIOC_S_INPUT, &in) && xioctl(fd, VIDIOC_ENUMINPUT, &in))
		fprintf(stderr, "note: no S_INPUT (patches/0003 handles it)\n");
	else
		printf("input %u: %s\n", input, in.name);

	if (pnum && pden) {
		struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };

		xioctl(fd, VIDIOC_G_PARM, &parm);
		parm.parm.capture.timeperframe.numerator = pnum;
		parm.parm.capture.timeperframe.denominator = pden;
		if (xioctl(fd, VIDIOC_S_PARM, &parm))
			fprintf(stderr, "S_PARM %u/%u failed: %s\n", pnum, pden, strerror(errno));
		else
			printf("requested %u/%u -> got %u/%u\n", pnum, pden,
			       parm.parm.capture.timeperframe.numerator,
			       parm.parm.capture.timeperframe.denominator);
	}

	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };

	fmt.fmt.pix_mp.width = w;
	fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.pixelformat = fourcc;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt)) {
		fprintf(stderr, "S_FMT: %s\n", strerror(errno));
		return 1;
	}
	w = fmt.fmt.pix_mp.width;
	h = fmt.fmt.pix_mp.height;
	fourcc = fmt.fmt.pix_mp.pixelformat;
	printf("fmt %c%c%c%c %ux%u planes=%u sizeimage=%u bytesperline=%u\n",
	       fourcc & 0xff, (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff,
	       w, h, fmt.fmt.pix_mp.num_planes, fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
	       fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

	struct v4l2_requestbuffers req = {
		.count = nbuf,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};

	if (xioctl(fd, VIDIOC_REQBUFS, &req) || req.count < 2) {
		fprintf(stderr, "REQBUFS: %s (count %u)\n", strerror(errno), req.count);
		return 1;
	}

	for (unsigned int i = 0; i < req.count; i++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
			.length = 1,
			.m.planes = planes,
		};

		if (xioctl(fd, VIDIOC_QUERYBUF, &b)) {
			fprintf(stderr, "QUERYBUF %u: %s\n", i, strerror(errno));
			return 1;
		}
		bufs[i].length = b.m.planes[0].length;
		bufs[i].start = mmap(NULL, bufs[i].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				     b.m.planes[0].m.mem_offset);
		if (bufs[i].start == MAP_FAILED) {
			fprintf(stderr, "mmap %u: %s\n", i, strerror(errno));
			return 1;
		}
	}

	for (unsigned int i = 0; i < req.count; i++) {
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
			.length = 1,
			.m.planes = planes,
		};

		if (xioctl(fd, VIDIOC_QBUF, &b)) {
			fprintf(stderr, "QBUF %u: %s\n", i, strerror(errno));
			return 1;
		}
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (xioctl(fd, VIDIOC_STREAMON, &type)) {
		fprintf(stderr, "STREAMON: %s\n", strerror(errno));
		return 1;
	}

	const size_t gapmax = ((size_t)seconds + 8) * 4096;
	double *gaps = malloc(sizeof(double) * gapmax);
	double t0 = now_sec(), tlast = t0, tprev = 0;
	unsigned long frames = 0, per_sec = 0, timeouts = 0;
	size_t ngap = 0;
	int rc = 0;

	for (;;) {
		const double t = now_sec();

		if (t - t0 >= seconds)
			break;

		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 1000);

		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll: %s\n", strerror(errno));
			rc = 1;
			break;
		}
		if (pr == 0) {
			timeouts++;
			if (!quiet)
				printf("  [%.1fs] no frame for >1s (frames=%lu)\n", t - t0, frames);
			continue;
		}

		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.length = 1,
			.m.planes = planes,
		};

		if (xioctl(fd, VIDIOC_DQBUF, &b)) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "DQBUF: %s\n", strerror(errno));
			rc = 1;
			break;
		}

		const double tn = now_sec();

		if (frames && ngap < gapmax)
			gaps[ngap++] = tn - tprev;
		tprev = tn;
		frames++;
		per_sec++;

		if (xioctl(fd, VIDIOC_QBUF, &b)) {
			fprintf(stderr, "QBUF: %s\n", strerror(errno));
			rc = 1;
			break;
		}

		if (tn - tlast >= 1.0) {
			if (!quiet)
				printf("  t=%5.1fs frames=%7lu this_second=%lu\n", tn - t0, frames,
				       per_sec);
			per_sec = 0;
			tlast = tn;
		}
	}

	const double t1 = now_sec();

	if (xioctl(fd, VIDIOC_STREAMOFF, &type))
		fprintf(stderr, "STREAMOFF: %s\n", strerror(errno));

	for (unsigned int i = 0; i < req.count; i++)
		munmap(bufs[i].start, bufs[i].length);
	close(fd);

	const double dt = t1 - t0;

	printf("RESULT dev=%s %ux%u %c%c%c%c frames=%lu wall=%.3fs fps=%.2f timeouts=%lu\n",
	       dev, w, h, fourcc & 0xff, (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff,
	       (fourcc >> 24) & 0xff, frames, dt, frames / dt, timeouts);

	if (ngap) {
		double *sorted = malloc(sizeof(double) * ngap);

		memcpy(sorted, gaps, sizeof(double) * ngap);
		qsort(sorted, ngap, sizeof(double), cmp_double);
		const double med = sorted[ngap / 2];
		unsigned long late = 0, verylate = 0;

		for (size_t i = 0; i < ngap; i++) {
			if (gaps[i] > med * 1.5)
				late++;
			if (gaps[i] > med * 3.0)
				verylate++;
		}
		printf("GAPS n=%zu median=%.6fs (%.2f fps) min=%.6f max=%.6f >1.5x=%lu >3x=%lu\n",
		       ngap, med, 1.0 / med, sorted[0], sorted[ngap - 1], late, verylate);
		free(sorted);
	}
	free(gaps);
	return rc;
}

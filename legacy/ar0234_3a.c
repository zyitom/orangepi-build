/*
 * ar0234_3a: run the Allwinner libisp 3A server (AE/AWB) for a vin video node.
 *
 * libisp and cedarc (libcdc_base) export different iniparser/dictionary
 * implementations under the same global symbol names, so whichever loads
 * first is used by both. Recorders keep them in separate processes and start
 * this helper instead of linking libisp themselves.
 *
 * libisp only reads /mnt/extsd/isp_param_config.bin. The hardware 3DNR needs
 * vertical blanking to update its reference frames and loses every frame at
 * 1920x1200@120 (16 blank lines), so the helper installs the parameter set
 * matching the capture mode:
 *   $AR0234_ISP_DIR/isp_param_3dnr.bin     default
 *   $AR0234_ISP_DIR/isp_param_no3dnr.bin   height >= 1200 and fps > 110
 * AR0234_ISP_DIR defaults to /mnt/extsd/ar0234; without those files the
 * active parameter file is left alone. vin refuses a second open of a busy
 * video node, so recorders pass the mode; otherwise it is queried.
 *
 * ar0234_3a [video_id [width height fps]]
 *   prints "ready" once started, stops on SIGTERM/SIGINT
 * build: gcc -O2 -o ar0234_3a ar0234_3a.c -lAWIspApi -lisp -lisp_ini -lpthread
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include <AWIspApi.h>

#define ISP_PARAM_FILE "/mnt/extsd/isp_param_config.bin"
#define PARAM_MAX (256 * 1024)

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static long read_file(const char *path, char *buf, long max)
{
	FILE *fp = fopen(path, "rb");
	long n;

	if (!fp)
		return -1;
	n = fread(buf, 1, max, fp);
	fclose(fp);
	return n;
}

/* copy the parameter set for the current mode over ISP_PARAM_FILE if it differs */
static void select_params(int video, unsigned int w, unsigned int h, unsigned int fps)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	const char *dir = getenv("AR0234_ISP_DIR");
	char dev[32], path[256];
	static char want[PARAM_MAX], have[PARAM_MAX];
	long nw, nh;
	int fd;

	if (!h) {
		snprintf(dev, sizeof(dev), "/dev/video%d", video);
		fd = open(dev, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			return;
		if (ioctl(fd, VIDIOC_G_FMT, &f)) {
			close(fd);
			return;
		}
		if (!ioctl(fd, VIDIOC_G_PARM, &parm) && parm.parm.capture.timeperframe.numerator)
			fps = parm.parm.capture.timeperframe.denominator /
			      parm.parm.capture.timeperframe.numerator;
		close(fd);
		w = f.fmt.pix_mp.width;
		h = f.fmt.pix_mp.height;
	}

	snprintf(path, sizeof(path), "%s/isp_param_%s.bin", dir ? dir : "/mnt/extsd/ar0234",
		 h >= 1200 && fps > 110 ? "no3dnr" : "3dnr");
	nw = read_file(path, want, PARAM_MAX);
	if (nw <= 0)
		return;
	nh = read_file(ISP_PARAM_FILE, have, PARAM_MAX);
	if (nh == nw && !memcmp(want, have, nw))
		return;

	FILE *fp = fopen(ISP_PARAM_FILE ".tmp", "wb");
	if (!fp || fwrite(want, 1, nw, fp) != (size_t)nw || fclose(fp) ||
	    rename(ISP_PARAM_FILE ".tmp", ISP_PARAM_FILE)) {
		perror("ar0234_3a: install " ISP_PARAM_FILE);
		return;
	}
	fprintf(stderr, "ar0234_3a: %ux%u@%u -> %s\n", w, h, fps, path);
}

int main(int argc, char **argv)
{
	int video = argc > 1 ? atoi(argv[1]) : 0;
	AWIspApi *isp;
	int id;

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	if (argc > 4)
		select_params(video, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
	else
		select_params(video, 0, 0, 0);
	isp = CreateAWIspApi();
	if (!isp || isp->ispApiInit() < 0) {
		fprintf(stderr, "ar0234_3a: libisp init failed\n");
		return 1;
	}
	id = isp->ispGetIspId(video);
	if (id < 0 || isp->ispStart(id) < 0) {
		fprintf(stderr, "ar0234_3a: start failed for video%d (isp %d)\n", video, id);
		return 1;
	}
	printf("ready isp%d\n", id);
	fflush(stdout);
	while (!stop)
		pause();
	isp->ispStop(id);
	isp->ispWaitToExit(id);
	isp->ispApiUnInit();
	DestroyAWIspApi(isp);
	return 0;
}

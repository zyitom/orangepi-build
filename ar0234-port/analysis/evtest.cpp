// experiment: watch sunxi_isp.0 subdev events, start/stop a 3A child per stream session
#include <csignal>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#define V4L2_EVENT_VIN_CLASS (V4L2_EVENT_PRIVATE_START | 0x100)
#define V4L2_EVENT_VIN_ISP_OFF (V4L2_EVENT_VIN_CLASS | 0x3)

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/v4l-subdev12";
	const char *child = argc > 2 ? argv[2] : nullptr;
	int fd = open(node, O_RDWR);
	if (fd < 0) { perror(node); return 1; }
	for (unsigned t : {unsigned(V4L2_EVENT_FRAME_SYNC), unsigned(V4L2_EVENT_VIN_ISP_OFF)}) {
		v4l2_event_subscription sub{};
		sub.type = t;
		if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) perror("subscribe");
	}
	pid_t pid = -1;
	for (;;) {
		pollfd p{fd, POLLPRI, 0};
		if (poll(&p, 1, -1) <= 0) continue;
		v4l2_event ev{};
		if (ioctl(fd, VIDIOC_DQEVENT, &ev)) { perror("dqevent"); continue; }
		v4l2_subdev_format f{};
		f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		ioctl(fd, VIDIOC_SUBDEV_G_FMT, &f);
		std::printf("%ld.%06ld type 0x%x seq %u data %u %u %u | isp pad0 %ux%u\n",
			    ev.timestamp.tv_sec, ev.timestamp.tv_nsec / 1000, ev.type, ev.sequence,
			    ev.u.data[0], ev.u.data[1], ev.u.data[2], f.format.width, f.format.height);
		std::fflush(stdout);
		if (!child) continue;
		if (ev.type == V4L2_EVENT_FRAME_SYNC && pid < 0) {
			pid = fork();
			if (pid == 0) { execl(child, child, "0", nullptr); _exit(127); }
			std::printf("started 3A pid %d\n", pid);
		} else if (ev.type == V4L2_EVENT_VIN_ISP_OFF && pid > 0) {
			kill(pid, SIGTERM);
			waitpid(pid, nullptr, 0);
			std::printf("stopped 3A\n");
			pid = -1;
		}
		std::fflush(stdout);
	}
}

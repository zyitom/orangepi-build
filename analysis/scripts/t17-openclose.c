/* openclose - plain open()/close() of a V4L2 node, no ioctls at all.
 * Used to reproduce HANDOFF 3.27: opening and closing a second video node
 * without streaming drags the first one from ~120 fps down to ~18 fps.
 *   openclose /dev/video4 [repeat]
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/video4";
	int n = argc > 2 ? atoi(argv[2]) : 1;
	int i;

	for (i = 0; i < n; i++) {
		int fd = open(dev, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
			return 1;
		}
		if (close(fd))
			fprintf(stderr, "close %s: %s\n", dev, strerror(errno));
		printf("open/close %s #%d ok\n", dev, i + 1);
	}
	return 0;
}

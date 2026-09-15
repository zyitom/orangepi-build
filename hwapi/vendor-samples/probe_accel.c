/* probe_accel: check that the G2D and NPU kernel drivers answer (no processing) */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct g2d_hardware_version { uint32_t g2d_version; uint32_t chip_version; };
#define G2D_CMD_QUERY_VERSION _IOR('G', 0x9F, struct g2d_hardware_version)

int main(void)
{
	int fd = open("/dev/g2d", O_RDWR);
	if (fd < 0) {
		perror("/dev/g2d");
	} else {
		struct g2d_hardware_version v = {0};
		int r = ioctl(fd, G2D_CMD_QUERY_VERSION, &v);
		printf("G2D: QUERY_VERSION r=%d g2d_version=0x%x chip_version=0x%x\n", r, v.g2d_version, v.chip_version);
		close(fd);
	}
	fd = open("/dev/vipcore", O_RDWR);
	printf("NPU: open /dev/vipcore %s\n", fd < 0 ? "failed" : "ok");
	if (fd >= 0)
		close(fd);
	return 0;
}

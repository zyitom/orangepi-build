/*
 * ispreg_spy: LD_PRELOAD shim that logs the ISP module enable registers
 * libisp pushes to the kernel through VIDIOC_VIN_ISP_LOAD_REG.
 *   gcc -O2 -shared -fPIC -o ispreg_spy.so ispreg_spy.c -ldl
 *   LD_PRELOAD=./ispreg_spy.so ar0234_3a 0      (log: $ISPSPY_LOG or stderr)
 * Offsets are into the load DRAM buffer: ISP_LOAD_REG_OFFSET (0x100) + reg.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

struct isp_table_reg_map {
	void *addr;
	unsigned int size;
};
#define VIDIOC_VIN_ISP_LOAD_REG _IOWR('V', BASE_VIDIOC_PRIVATE + 70, struct isp_table_reg_map)

static int (*real_ioctl)(int, unsigned long, ...);
static unsigned int last[4] = { ~0u, ~0u, ~0u, ~0u };
static unsigned long calls;
static FILE *out;

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (!out) {
		const char *p = getenv("ISPSPY_LOG");
		out = p ? fopen(p, "w") : stderr;
		if (!out)
			out = stderr;
	}
	{
		static unsigned long reqs[64], cnt[64];
		static unsigned long total;
		int i;
		for (i = 0; i < 64 && reqs[i] && reqs[i] != req; i++)
			;
		if (i < 64) {
			reqs[i] = req;
			cnt[i]++;
		}
		if (++total % 500 == 0) {
			fprintf(out, "ioctl histogram after %lu calls:", total);
			for (i = 0; i < 64 && reqs[i]; i++)
				fprintf(out, " %08lx:%lu", reqs[i], cnt[i]);
			fprintf(out, "\n");
			fflush(out);
		}
	}
	if (req == VIDIOC_VIN_ISP_LOAD_REG && arg) {
		struct isp_table_reg_map *m = arg;
		unsigned int v[4];
		calls++;
		if (m->addr && m->size >= 0x140) {
			memcpy(v, (char *)m->addr + 0x100 + 0x030, sizeof(v)); /* bypass0, bypass1, mode0, +0x3c */
			if (memcmp(v, last, sizeof(v))) {
				fprintf(out, "call %lu size %u bypass0 %08x bypass1 %08x mode0 %08x r3c %08x\n",
					calls, m->size, v[0], v[1], v[2], v[3]);
				fflush(out);
				memcpy(last, v, sizeof(v));
			}
		}
	}
	return real_ioctl(fd, req, arg);
}

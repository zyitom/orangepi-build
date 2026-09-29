/* ioctl_shim.c — LD_PRELOAD：解码 PVR bridge（cmd 0xc0206440）的完整包结构。
 * 包 = {bridgeID u32, funcID u32, pvParamIn ptr, pvParamOut ptr, inSize u32, outSize u32}
 * 每次调用打印：mod/func/inSize/outSize + 入参前 32B + 出参前 32B hexdump。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int (*real_ioctl)(int, unsigned long, void *);

static void hex(const unsigned char *p, int n)
{
	for (int i = 0; i < n; i++)
		fprintf(stderr, "%02x", p[i]);
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");

	int r = real_ioctl(fd, req, arg);

	if (req == 0xc0206440 && r >= 0 && arg) {
		uint32_t bid, fid, insz, outsz;
		uint64_t inptr = 0, outptr = 0;
		unsigned char *p = arg;
		memcpy(&bid, p, 4);
		memcpy(&fid, p + 4, 4);
		memcpy(&inptr, p + 8, 8);
		memcpy(&outptr, p + 16, 8);
		memcpy(&insz, p + 24, 4);
		memcpy(&outsz, p + 28, 4);
		fprintf(stderr, "[shim] mod=%u func=%u in=%u out=%u\n", bid, fid, insz, outsz);
		if (inptr && insz >= 4) {
			unsigned char b[24];
			if (insz < sizeof(b)) memcpy(b, (void *)(uintptr_t)inptr, insz);
			else memcpy(b, (void *)(uintptr_t)inptr, sizeof(b));
			fprintf(stderr, "  IN : "); hex(b, insz < 24 ? insz : 24); fprintf(stderr, "\n");
		}
		if (outptr && outsz >= 4) {
			unsigned char b[24];
			if (outsz < sizeof(b)) memcpy(b, (void *)(uintptr_t)outptr, outsz);
			else memcpy(b, (void *)(uintptr_t)outptr, sizeof(b));
			fprintf(stderr, "  OUT: "); hex(b, outsz < 24 ? outsz : 24); fprintf(stderr, "\n");
		}
	}
	return r;
}

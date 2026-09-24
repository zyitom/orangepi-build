/* Shared DMA-BUF CPU-cache-sync helper for the g2d probes (and any other
 * throwaway tool that talks to /dev/dma_heap + /dev/g2d on this board).
 *
 * WHY THIS EXISTS
 *   Every probe used to hard-code the *mainline* DMA_BUF_SYNC_* flag layout.
 *   This vendor 6.6 kernel (uapi/linux/dma-buf.h) still uses the legacy one,
 *   so the ioctl failed with EINVAL on every single call and -- because the
 *   return value was ignored -- "cache maintenance" silently did nothing
 *   (analysis/round9/REPORT.md, "DMA_BUF_IOCTL_SYNC 旧编码"; board-measured:
 *   flags 0/4/12/13 EINVAL, 1..3/5..7 OK).
 *
 *   The two layouts:
 *     legacy (this kernel):   READ = 1<<0, WRITE = 2<<0, RW = 3,
 *                             START = 0<<2, END = 1<<2, valid mask = 7
 *     mainline (6.12+):       READ = 1<<2, WRITE = 2<<2, RW = 12,
 *                             START = 0<<0, END = 1<<0, valid mask = 13
 *   They are mutually exclusive (legacy RW = 3 is outside the mainline mask,
 *   mainline RW = 12 is outside the legacy mask), so the first sync probes
 *   both and remembers which one the kernel accepts.
 *
 * USAGE
 *   syncbuf(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW)   before a CPU access
 *   syncbuf(fd, DMA_BUF_SYNC_END   | DMA_BUF_SYNC_RW)   after it
 *   The macro names are the *logical* access phase, NOT the kernel's bit
 *   layout. The direction (READ/WRITE/RW) is mandatory: the kernel rejects 0
 *   and END-alone with EINVAL. Any failure prints and exits(2) -- a silently
 *   no-op sync is exactly the bug this header exists to prevent.
 *   sync_() is an alias kept for the older probes that spell it that way.
 */
#ifndef G2D_PROBES_DMABUF_SYNC_H
#define G2D_PROBES_DMABUF_SYNC_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/types.h>

struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

/* Logical flags (the call-site vocabulary), not the kernel bit layout. */
#define DMA_BUF_SYNC_READ  1u
#define DMA_BUF_SYNC_WRITE 2u
#define DMA_BUF_SYNC_RW    (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START 4u
#define DMA_BUF_SYNC_END   8u

/* 0 = legacy, 1 = mainline, -1 = not probed yet (per process). */
static int dmabuf_sync_encoding = -1;

static unsigned long long dmabuf_sync_flags(int enc, unsigned access)
{
	const int mainline = (enc == 1);
	unsigned long long dir, phase;

	if (access & DMA_BUF_SYNC_WRITE)
		dir = mainline ? (2ull << 2) : (2ull << 0);
	else
		dir = mainline ? (1ull << 2) : (1ull << 0);
	phase = mainline ? ((access & DMA_BUF_SYNC_END) ? 1ull : 0ull)
			 : ((access & DMA_BUF_SYNC_END) ? (1ull << 2) : 0ull);
	return dir | phase;
}

/* Returns 0 on success; prints and exits(2) on any failure. */
static inline int syncbuf(int fd, unsigned access)
{
	struct dma_buf_sync s;

	if (!(access & DMA_BUF_SYNC_RW)) {
		fprintf(stderr, "syncbuf: flags 0x%x have no direction bit; "
				"the kernel rejects that with EINVAL\n", access);
		exit(2);
	}
	if (dmabuf_sync_encoding < 0) {
		const int order[2] = { 0, 1 };
		int i;
		for (i = 0; i < 2; ++i) {
			s.flags = dmabuf_sync_flags(order[i],
						    DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0) {
				dmabuf_sync_encoding = order[i];
				break;
			}
			if (errno != EINVAL) {
				fprintf(stderr, "syncbuf: probe ioctl: %s\n", strerror(errno));
				exit(2);
			}
		}
		if (dmabuf_sync_encoding < 0) {
			fprintf(stderr, "syncbuf: kernel accepted neither the legacy nor "
					"the mainline DMA_BUF_SYNC encoding\n");
			exit(2);
		}
	}
	s.flags = dmabuf_sync_flags(dmabuf_sync_encoding, access);
	if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0)
		return 0;
	fprintf(stderr, "syncbuf: DMA_BUF_IOCTL_SYNC flags=0x%llx: %s\n",
		(unsigned long long)s.flags, strerror(errno));
	exit(2);
}

#define sync_ syncbuf

#endif /* G2D_PROBES_DMABUF_SYNC_H */

// G2D self-test — deliberately INDEPENDENT of the ar0234 capture library.
// Uses only /dev/dma_heap/system + /dev/g2d, following the vendor 2D guide.
// Goal: decide whether G2D copies correctly (byte-exact) when used correctly.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include <stdbool.h>

#include "sunxi-g2d.h"
struct dma_heap_allocation_data { __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags; };
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#include "dmabuf_sync.h"


static int heap_alloc(size_t len, void **map)
{
	struct dma_heap_allocation_data d;
	int h = open("/dev/dma_heap/system", O_RDONLY);
	if (h < 0) { perror("open dma_heap"); return -1; }
	memset(&d, 0, sizeof d);
	d.len = len;
	d.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(h, DMA_HEAP_IOCTL_ALLOC, &d) < 0) { perror("DMA_HEAP_IOCTL_ALLOC"); close(h); return -1; }
	close(h);
	*map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd, 0);
	if (*map == MAP_FAILED) { perror("mmap"); close(d.fd); return -1; }
	return (int)d.fd;
}

static void fill(uint8_t *p, size_t n, unsigned seed)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t)(i * 31u + seed * 17u + (i >> 8) * 7u);
}

struct cmp { size_t diff; int maxd; int exact; };

static struct cmp compare(const uint8_t *a, const uint8_t *b, size_t n)
{
	struct cmp r = {0, 0, 1};
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
			r.diff++; r.exact = 0;
			if (d > r.maxd) r.maxd = d;
		}
	}
	return r;
}

// one BITBLT: returns ioctl result
static int blit(int g2d, int sfd, int dfd, unsigned fmt, unsigned w, unsigned h,
		unsigned dw, unsigned dh, unsigned flag, int bbuff, unsigned use_phy)
{
	g2d_blt_h b;
	memset(&b, 0, sizeof b);
	b.flag_h = flag;
	b.src_image_h.fd = sfd;
	b.src_image_h.format = fmt;
	b.src_image_h.width = w;
	b.src_image_h.height = h;
	b.src_image_h.clip_rect.x = 0; b.src_image_h.clip_rect.y = 0;
	b.src_image_h.clip_rect.w = w; b.src_image_h.clip_rect.h = h;
	b.src_image_h.use_phy_addr = use_phy;
	b.src_image_h.bbuff = bbuff;
	b.dst_image_h = b.src_image_h;
	b.dst_image_h.fd = dfd;
	b.dst_image_h.width = dw;
	b.dst_image_h.height = dh;
	b.dst_image_h.clip_rect.w = dw;
	b.dst_image_h.clip_rect.h = dh;
	int rc = ioctl(g2d, G2D_CMD_BITBLT_H, &b);
	if (rc < 0) fprintf(stderr, "   BITBLT failed: %s\n", strerror(errno));
	return rc;
}

int main(void)
{
	int g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	struct g2d_hardware_version v;
	memset(&v, 0, sizeof v);
	if (ioctl(g2d, G2D_CMD_QUERY_VERSION, &v) == 0)
		printf("g2d version 0x%llx\n", (unsigned long long)v.g2d_version);

	/* ---- T1..T3: NV12 1920x1080 plain copy, dedicated heap buffers,
	 *      CORRECT dma-buf sync order (start/end around each CPU access) ---- */
	const unsigned W = 1920, H = 1080;
	const size_t NV12 = (size_t)W * H * 3 / 2;
	void *sm, *dm;
	int sfd = heap_alloc(NV12, &sm);
	int dfd = heap_alloc(NV12, &dm);
	if (sfd < 0 || dfd < 0) return 1;
	printf("heap: src fd=%d dst fd=%d size=%zu\n", sfd, dfd, NV12);

	for (int it = 1; it <= 3; it++) {
		/* CPU writes src: START before, END after */
		syncbuf(sfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		fill(sm, NV12, (unsigned)it * 13u);
		syncbuf(sfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);

		/* CPU poisons dst so we can tell whether G2D wrote it */
		syncbuf(dfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		memset(dm, 0xAA, NV12);
		syncbuf(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);

		int rc = blit(g2d, sfd, dfd, G2D_FORMAT_YUV420UVC_U1V1U0V0, W, H, W, H,
			      G2D_BLT_NONE_H, 1, 0);

		/* CPU reads dst: START before, END after (this is what we got wrong) */
		syncbuf(dfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		struct cmp c = compare((const uint8_t *)sm, (const uint8_t *)dm, NV12);
		uint8_t *d = dm;
		syncbuf(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);

		printf("T1.%d NV12 1920x1080 copy  ioctl rc=%d  differing=%zu/%zu maxdelta=%d  %s"
		       "   src[0..3]=%02x%02x%02x%02x dst[0..3]=%02x%02x%02x%02x\n",
		       it, rc, c.diff, NV12, c.maxd, c.exact ? "BYTE-EXACT" : "MISMATCH",
		       ((uint8_t *)sm)[0], ((uint8_t *)sm)[1], ((uint8_t *)sm)[2], ((uint8_t *)sm)[3],
		       d[0], d[1], d[2], d[3]);
	}

	/* ---- T4: same copy but bbuff = 0 (doc example does not set it) ---- */
	{
		syncbuf(sfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		fill(sm, NV12, 99);
		syncbuf(sfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		syncbuf(dfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		memset(dm, 0xAA, NV12);
		syncbuf(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		int rc = blit(g2d, sfd, dfd, G2D_FORMAT_YUV420UVC_U1V1U0V0, W, H, W, H,
			      G2D_BLT_NONE_H, 0, 0);
		syncbuf(dfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		struct cmp c = compare((const uint8_t *)sm, (const uint8_t *)dm, NV12);
		syncbuf(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		printf("T4   NV12 copy with bbuff=0        ioctl rc=%d  differing=%zu maxdelta=%d  %s\n",
		       rc, c.diff, c.maxd, c.exact ? "BYTE-EXACT" : "MISMATCH");
	}

	/* ---- T5: ARGB8888 1920x1080 plain copy (doc's format) ---- */
	{
		size_t n = (size_t)W * H * 4;
		void *s2m, *d2m;
		int s2 = heap_alloc(n, &s2m), d2 = heap_alloc(n, &d2m);
		if (s2 >= 0 && d2 >= 0) {
			syncbuf(s2, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			fill(s2m, n, 5);
			syncbuf(s2, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			syncbuf(d2, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(d2m, 0xAA, n);
			syncbuf(d2, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			int rc = blit(g2d, s2, d2, G2D_FORMAT_ARGB8888, W, H, W, H,
				      G2D_BLT_NONE_H, 1, 0);
			syncbuf(d2, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			struct cmp c = compare((const uint8_t *)s2m, (const uint8_t *)d2m, n);
			syncbuf(d2, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			printf("T5   ARGB8888 1920x1080 copy      ioctl rc=%d  differing=%zu/%zu maxdelta=%d  %s\n",
			       rc, c.diff, n, c.maxd, c.exact ? "BYTE-EXACT" : "MISMATCH");
			munmap(s2m, n); close(s2); munmap(d2m, n); close(d2);
		}
	}

	/* ---- T6: ROT_90 geometry on a small ARGB pattern (proves real work) ---- */
	{
		const unsigned w = 64, h = 32;	/* rotate -> 32 x 64 */
		size_t n = (size_t)w * h * 4;
		void *sm2, *dm2;
		int s3 = heap_alloc(n, &sm2), d3 = heap_alloc(n, &dm2);
		if (s3 >= 0 && d3 >= 0) {
			syncbuf(s3, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			uint32_t *p = sm2;
			for (unsigned y = 0; y < h; y++)
				for (unsigned x = 0; x < w; x++)
					p[y * w + x] = 0xFF000000u | (y << 8) | x;
			syncbuf(s3, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			syncbuf(d3, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(dm2, 0xAA, n);
			syncbuf(d3, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			int rc = blit(g2d, s3, d3, G2D_FORMAT_ARGB8888, w, h, h, w,
				      G2D_ROT_90, 1, 0);
			syncbuf(d3, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			uint32_t *q = dm2;
			printf("T6   ROT_90 %ux%u -> %ux%u      ioctl rc=%d   dst[0,0]=%08x dst[0,dw-1]=%08x dst[dh-1,0]=%08x\n",
			       w, h, h, w, rc, q[0], q[h - 1], q[(w - 1) * h + 0]);
			syncbuf(d3, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			munmap(sm2, n); close(s3); munmap(dm2, n); close(d3);
		}
	}

	munmap(sm, NV12); close(sfd);
	munmap(dm, NV12); close(dfd);
	close(g2d);
	printf("done\n");
	return 0;
}

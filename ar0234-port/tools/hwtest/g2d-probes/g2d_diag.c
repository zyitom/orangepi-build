// G2D diagnostic: is the copy mismatch G2D's doing, or the harness?
// Control = CPU memcpy into a third heap buffer (must be byte-exact).
// Then localise WHICH bytes differ after a G2D copy.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <linux/ioctl.h>

#include "sunxi-g2d.h"

struct dma_heap_allocation_data { __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags; };
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#include "dmabuf_sync.h"


static int heap_alloc(size_t len, void **map)
{
	struct dma_heap_allocation_data d; int h = open("/dev/dma_heap/system", O_RDONLY);
	if (h < 0) { perror("open dma_heap"); return -1; }
	memset(&d, 0, sizeof d); d.len = len; d.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(h, DMA_HEAP_IOCTL_ALLOC, &d) < 0) { perror("heap alloc"); close(h); return -1; }
	close(h);
	*map = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, d.fd, 0);
	if (*map == MAP_FAILED) { perror("mmap"); close(d.fd); return -1; }
	return (int)d.fd;
}

static int g2d_copy(int g2d, int sfd, int dfd, unsigned fmt, unsigned w, unsigned h)
{
	g2d_blt_h b; memset(&b, 0, sizeof b);
	b.flag_h = G2D_BLT_NONE_H;
	b.src_image_h.fd = sfd; b.src_image_h.format = fmt;
	b.src_image_h.width = w; b.src_image_h.height = h;
	b.src_image_h.clip_rect.w = w; b.src_image_h.clip_rect.h = h;
	b.src_image_h.bbuff = 1; b.src_image_h.use_phy_addr = 0;
	b.dst_image_h = b.src_image_h; b.dst_image_h.fd = dfd;
	return ioctl(g2d, G2D_CMD_BITBLT_H, &b);
}

static void report(const char *tag, const uint8_t *a, const uint8_t *b, size_t n, unsigned w, unsigned h)
{
	size_t diff = 0, first = n, last = 0; int maxd = 0;
	size_t shown = 0;
	printf("  [%s] ", tag);
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			int d = a[i] > b[i] ? a[i]-b[i] : b[i]-a[i];
			if (d > maxd) maxd = d;
			diff++; if (i < first) first = i; last = i;
			if (shown < 6) {
				if (w) printf("\n     off=%zu (row %zu col %zu ch %zu) 0x%02x->0x%02x",
					      i, i/((size_t)w*4), (i%((size_t)w*4))/4, i%4, a[i], b[i]);
				else printf("\n     off=%zu 0x%02x->0x%02x", i, a[i], b[i]);
				shown++;
			}
		}
	}
	printf("\n     differing=%zu/%zu (%.2f%%) maxdelta=%d first=%zu last=%zu  %s\n",
	       diff, n, 100.0*diff/(double)n, maxd, first, last,
	       diff ? "MISMATCH" : "BYTE-EXACT");
	if (w && diff) {
		size_t yb=0, uvb=0; unsigned rowsdiff = 0; size_t prevrow = (size_t)-1;
		for (size_t i = 0; i < n; i++) if (a[i]!=b[i]) {
			size_t row = i/(size_t)w;
			if (row != prevrow) { rowsdiff++; prevrow = row; }
			if (i < (size_t)w*h) yb++; else uvb++;
		}
		printf("     Y plane diffs=%zu   UV plane diffs=%zu   rows touched=%u / %u\n", yb, uvb, rowsdiff, h);
	}
	if (!w && diff) {
		size_t ch[4] = {0,0,0,0};
		for (size_t i = 0; i < n; i++) if (a[i]!=b[i]) ch[i%4]++;
		printf("     per-channel diffs: B=%zu G=%zu R=%zu A=%zu\n", ch[0], ch[1], ch[2], ch[3]);
	}
}

int main(void)
{
	int g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	struct g2d_hardware_version v; memset(&v, 0, sizeof v);
	ioctl(g2d, G2D_CMD_QUERY_VERSION, &v);
	printf("g2d 0x%llx  uid=%d\n", (unsigned long long)v.g2d_version, getuid());

	/* ---------- ARGB8888 1920x1080 ---------- */
	const unsigned W = 1920, H = 1080;
	size_t n = (size_t)W * H * 4;
	void *sm, *dm, *rm;
	int sfd = heap_alloc(n, &sm), dfd = heap_alloc(n, &dm), rfd = heap_alloc(n, &rm);
	if (sfd < 0 || dfd < 0 || rfd < 0) return 1;

	syncbuf(sfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	for (size_t i = 0; i < n; i++) ((uint8_t*)sm)[i] = (uint8_t)(i*31u + (i>>8)*7u);
	syncbuf(sfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);

	printf("\n=== ARGB8888 1920x1080 : CONTROL (CPU memcpy) ===\n");
	syncbuf(rfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	memcpy(rm, sm, n);
	syncbuf(rfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	syncbuf(rfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	report("src vs CPU-copy", sm, rm, n, 0, 0);
	syncbuf(rfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);

	printf("\n=== ARGB8888 1920x1080 : G2D copy ===\n");
	syncbuf(dfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	memset(dm, 0xAA, n);
	syncbuf(dfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	int rc = g2d_copy(g2d, sfd, dfd, G2D_FORMAT_ARGB8888, W, H);
	printf("  ioctl rc=%d\n", rc);
	syncbuf(dfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	report("src vs G2D-copy", sm, dm, n, 0, 0);
	syncbuf(dfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);

	/* ---------- NV12 1920x1080 ---------- */
	size_t nv = (size_t)W*H*3/2;
	void *s2, *d2;
	int s2f = heap_alloc(nv, &s2), d2f = heap_alloc(nv, &d2);
	syncbuf(s2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	for (size_t i = 0; i < nv; i++) ((uint8_t*)s2)[i] = (uint8_t)(i*31u + (i>>8)*7u);
	syncbuf(s2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	syncbuf(d2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	memset(d2, 0xAA, nv);
	syncbuf(d2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	printf("\n=== NV12 1920x1080 : G2D copy ===\n");
	rc = g2d_copy(g2d, s2f, d2f, G2D_FORMAT_YUV420UVC_U1V1U0V0, W, H);
	printf("  ioctl rc=%d\n", rc);
	syncbuf(d2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	report("src vs G2D-copy", s2, d2, nv, W, H);
	syncbuf(d2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);

	printf("\ndone\n");
	return 0;
}

/* Is there a bit-exact way to move an NV12 frame with G2D, and how do you
 * annotate one?
 *
 *   K. NV12 -> NV12 as TWO G2D_FORMAT_Y8 blits.  The NV12 buffer is exactly
 *      1920x1620 bytes of 8-bit data, so a Y8 image with width=1920, height=1620
 *      and clip_rect.y = 0 / 1080 selects the luma plane / the chroma plane, and
 *      a Y8 blit was measured bit-exact (0/2073600, no range clamp).  If this
 *      lands at 0 differing bytes it is the workaround for the chroma filter.
 *   L. G2D_CMD_FILLRECT_H on an NV12 target (the annotation use case): what
 *      colour encoding does the driver expect, and does it write both planes?
 *
 * Build on the board:  gcc -O2 -o /tmp/g2d_work g2d_work.c
 */
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

#define W 1920
#define H 1080
#define YLEN ((size_t)W * H)
#define UVLEN ((size_t)W * H / 2)
#define NV12 (YLEN + UVLEN)
#define ROWS (H + H / 2)          /* the buffer is exactly W x ROWS bytes */

static int g2d;

static int heap_alloc(size_t len, void **map)
{
	struct dma_heap_allocation_data d; int h = open("/dev/dma_heap/system", O_RDONLY);
	if (h < 0) return -1;
	memset(&d, 0, sizeof d); d.len = len; d.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(h, DMA_HEAP_IOCTL_ALLOC, &d) < 0) { close(h); return -1; }
	close(h);
	*map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd, 0);
	if (*map == MAP_FAILED) { close(d.fd); return -1; }
	return (int)d.fd;
}

static int y8_blt(int sfd, int dfd, unsigned y0, unsigned hh)
{
	g2d_blt_h b; memset(&b, 0, sizeof b);
	b.flag_h = G2D_BLT_NONE_H;
	b.src_image_h.fd = sfd; b.src_image_h.format = G2D_FORMAT_Y8;
	b.src_image_h.width = W; b.src_image_h.height = ROWS;
	b.src_image_h.clip_rect.x = 0; b.src_image_h.clip_rect.y = y0;
	b.src_image_h.clip_rect.w = W; b.src_image_h.clip_rect.h = hh;
	b.src_image_h.bbuff = 1; b.src_image_h.use_phy_addr = 0;
	b.dst_image_h = b.src_image_h; b.dst_image_h.fd = dfd;
	return ioctl(g2d, G2D_CMD_BITBLT_H, &b);
}

static int nv12_blt(int sfd, int dfd, g2d_fmt_enh f)
{
	g2d_blt_h b; memset(&b, 0, sizeof b);
	b.flag_h = G2D_BLT_NONE_H;
	b.src_image_h.fd = sfd; b.src_image_h.format = f;
	b.src_image_h.width = W; b.src_image_h.height = H;
	b.src_image_h.clip_rect.w = W; b.src_image_h.clip_rect.h = H;
	b.src_image_h.bbuff = 1; b.src_image_h.use_phy_addr = 0;
	b.dst_image_h = b.src_image_h; b.dst_image_h.fd = dfd;
	return ioctl(g2d, G2D_CMD_BITBLT_H, &b);
}

static uint32_t xs = 0x27d4eb2fu;
static unsigned char rnd8(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return (unsigned char)(xs >> 11); }

int main(void)
{
	uint8_t *s, *d;
	int sf, df;
	g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	sf = heap_alloc(NV12, (void **)&s); df = heap_alloc(NV12, (void **)&d);
	if (sf < 0 || df < 0) { perror("dma_heap"); return 1; }
	printf("NV12 buffer = %zu bytes = %ux%u 8-bit rows; Y rows 0..%u, chroma rows %u..%u\n",
	       (size_t)NV12, W, ROWS, H - 1, H, ROWS - 1);

	/* worst case on purpose: full-range random, including values < 16 */
	sync_(sf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	{
		size_t i;
		for (i = 0; i < NV12; ++i) s[i] = rnd8();
	}
	sync_(sf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	memset(d, 0xA5, NV12);
	sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);

	printf("\n== K. NV12 -> NV12 as two Y8 blits (full-range random source) ==\n");
	int r1 = y8_blt(sf, df, 0, H);
	int r2 = y8_blt(sf, df, H, H / 2);
	sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	{
		size_t i, dy = 0, duv = 0, my = 0, muv = 0;
		for (i = 0; i < YLEN; ++i) {
			if (d[i] == s[i]) continue;
			++dy;
			unsigned dv = d[i] > s[i] ? d[i] - s[i] : s[i] - d[i];
			if (dv > my) my = dv;
		}
		for (i = YLEN; i < NV12; ++i) {
			if (d[i] == s[i]) continue;
			++duv;
			unsigned dv = d[i] > s[i] ? d[i] - s[i] : s[i] - d[i];
			if (dv > muv) muv = dv;
		}
		printf("   rc=%d/%d  Y differing %zu/%zu (maxdelta %zu)  UV differing %zu/%zu (maxdelta %zu)\n",
		       r1, r2, dy, YLEN, my, duv, UVLEN, muv);
		printf("   => %s\n", (dy == 0 && duv == 0) ? "BIT-EXACT NV12 COPY WORKS" : "not bit-exact");
	}
	sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);

	/* for comparison, the same data through the native NV12 -> NV12 blit */
	memset(d, 0xA5, NV12);
	sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	int r0 = nv12_blt(sf, df, G2D_FORMAT_YUV420UVC_V1U1V0U0);
	sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	{
		size_t i, dy = 0, duv = 0;
		for (i = 0; i < YLEN; ++i) dy += d[i] != s[i];
		for (i = YLEN; i < NV12; ++i) duv += d[i] != s[i];
		printf("   (reference) one YUV420UVC_V1U1V0U0 blit rc=%d: Y differing %zu/%zu, UV %zu/%zu\n",
		       r0, dy, YLEN, duv, UVLEN);
	}
	sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);

	/* ---- L: fillrect on an NV12 target ---------------------------------- */
	printf("\n== L. G2D_CMD_FILLRECT_H on NV12 (annotation), whole-image rect ==\n");
	{
		unsigned colors[3] = {0x00108080u, 0x00FF8040u, 0x8010A0C0u};
		int k;
		for (k = 0; k < 3; ++k) {
			g2d_fillrect_h fr;
			unsigned char yv, uv0, uv1;
			size_t i, ny = 0, nuv0 = 0, nuv1 = 0;
			memset(&fr, 0, sizeof fr);
			fr.dst_image_h.fd = df;
			fr.dst_image_h.format = G2D_FORMAT_YUV420UVC_V1U1V0U0;
			fr.dst_image_h.width = W; fr.dst_image_h.height = H;
			fr.dst_image_h.clip_rect.w = W; fr.dst_image_h.clip_rect.h = H;
			fr.dst_image_h.bbuff = 1; fr.dst_image_h.use_phy_addr = 0;
			fr.dst_image_h.color = colors[k];
			sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(d, 0xA5, NV12);
			sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			int rc = ioctl(g2d, G2D_CMD_FILLRECT_H, &fr);
			sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			yv = d[0]; uv0 = d[YLEN]; uv1 = d[YLEN + 1];
			for (i = 0; i < YLEN; ++i) ny += d[i] != yv;
			for (i = YLEN; i < NV12; i += 2) { nuv0 += d[i] != uv0; nuv1 += d[i + 1] != uv1; }
			printf("   color=0x%06x rc=%d  Y=%3u (non-uniform %zu) U@even=%3u (non-uniform %zu) V@odd=%3u (non-uniform %zu)\n",
			       colors[k], rc, yv, ny, uv0, nuv0, uv1, nuv1);
			sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		}
		/* sub-rectangle: the actual annotation case */
		{
			g2d_fillrect_h fr;
			size_t i, inside = 0, outside = 0;
			memset(&fr, 0, sizeof fr);
			fr.dst_image_h.fd = df;
			fr.dst_image_h.format = G2D_FORMAT_YUV420UVC_V1U1V0U0;
			fr.dst_image_h.width = W; fr.dst_image_h.height = H;
			fr.dst_image_h.clip_rect.x = 100; fr.dst_image_h.clip_rect.y = 64;
			fr.dst_image_h.clip_rect.w = 256; fr.dst_image_h.clip_rect.h = 64;
			fr.dst_image_h.bbuff = 1; fr.dst_image_h.use_phy_addr = 0;
			fr.dst_image_h.color = 0x00F08040u;
			sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(d, 0x55, NV12);
			sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			int rc = ioctl(g2d, G2D_CMD_FILLRECT_H, &fr);
			sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			printf("   rect(100,64,256x64) color=0x00F08040 rc=%d\n", rc);
			for (i = 0; i < YLEN; ++i) {
				size_t x = i % W, y = i / W;
				int in = (x >= 100 && x < 356 && y >= 64 && y < 128);
				if (in) inside += d[i] != 0x55; else outside += d[i] != 0x55;
			}
			printf("   Y plane: bytes changed inside rect %zu (of %zu), outside rect %zu (must be 0)\n",
			       inside, (size_t)256 * 64, outside);
			printf("   chroma at (row 32, pair 50..52 x2) = %02x %02x %02x %02x\n",
			       d[YLEN + 32 * W + 100], d[YLEN + 32 * W + 101],
			       d[YLEN + 32 * W + 102], d[YLEN + 32 * W + 103]);
			sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		}
	}

	printf("\ndone\n");
	return 0;
}

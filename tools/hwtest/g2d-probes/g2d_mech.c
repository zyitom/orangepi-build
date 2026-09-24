/* Follow-up to g2d_mark.c: pin down the chroma filter, and find out which
 * operations are actually usable.
 *
 *   D. horizontally smooth / vertically constant chroma -> where exactly does the
 *      filter change bytes, and by how much?
 *   E. G2D_FORMAT_Y8 (single 8-bit plane) copy -> is a luma-only blit bit-exact?
 *   F. ARGB8888 -> NV12 (the vendor-documented g2d_format_conv path): constant
 *      colour in, read back Y / U / V.  Also checks the U-V interleave order.
 *   G. NV12 -> ARGB8888 (the other documented path).
 *
 * Build on the board:  gcc -O2 -o /tmp/g2d_mech g2d_mech.c
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
#include <math.h>
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
#define ARGB ((size_t)W * H * 4)

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

static int blt(int sfd, int dfd, g2d_fmt_enh sfmt, g2d_fmt_enh dfmt, unsigned w, unsigned h)
{
	g2d_blt_h b; memset(&b, 0, sizeof b);
	b.flag_h = G2D_BLT_NONE_H;
	b.src_image_h.fd = sfd; b.src_image_h.format = sfmt;
	b.src_image_h.width = w; b.src_image_h.height = h;
	b.src_image_h.clip_rect.w = w; b.src_image_h.clip_rect.h = h;
	b.src_image_h.bbuff = 1; b.src_image_h.use_phy_addr = 0;
	b.dst_image_h = b.src_image_h; b.dst_image_h.fd = dfd; b.dst_image_h.format = dfmt;
	return ioctl(g2d, G2D_CMD_BITBLT_H, &b);
}

static uint32_t xs = 0x9e3779b9u;
static unsigned char rnd8(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return (unsigned char)(xs >> 11); }

/* ---------------------------------------------------------------- D + E + F/G */
int main(void)
{
	uint8_t *s, *d, *a, *ad;
	int sf, df, af, adf;
	g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	sf = heap_alloc(NV12, (void **)&s);
	df = heap_alloc(NV12, (void **)&d);
	af = heap_alloc(ARGB, (void **)&a);
	adf = heap_alloc(ARGB, (void **)&ad);
	if (sf < 0 || df < 0 || af < 0 || adf < 0) { perror("dma_heap"); return 1; }

	/* ---- D: vertical-constant chroma, smooth horizontally ---------------- */
	{
		const size_t plateau = 64;
		size_t i, diff = 0, maxd = 0, interior_diff = 0, edge_diff = 0;
		sync_(sf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		memset(s, 0x80, YLEN);
		/* within one row: plateaus of 64 bytes, period 1024 bytes (NOT 256) */
		for (i = 0; i < UVLEN; ++i) {
			size_t col = i % W;
			s[YLEN + i] = (uint8_t)(0x80 + ((col % 1024) / plateau) * 3);
		}
		sync_(sf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		memset(d, 0xA5, NV12);
		sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		int rc = blt(sf, df, G2D_FORMAT_YUV420UVC_U1V1U0V0, G2D_FORMAT_YUV420UVC_U1V1U0V0, W, H);
		sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		printf("== D. vertically constant, horizontally smooth chroma (64 B plateaus, period 1024) ==\n");
		printf("   rc=%d\n", rc);
		for (i = 0; i < UVLEN; ++i) {
			unsigned dv;
			if (d[YLEN + i] == s[YLEN + i]) continue;
			++diff;
			dv = d[YLEN + i] > s[YLEN + i] ? d[YLEN + i] - s[YLEN + i] : s[YLEN + i] - d[YLEN + i];
			if (dv > maxd) maxd = dv;
			/* distance to the nearest 64-byte plateau boundary, in the row */
			{
				size_t col = i % W, off = col % plateau;
				if (off >= 4 && off <= plateau - 5) ++interior_diff; else ++edge_diff;
			}
		}
		printf("   UV differing %zu/%zu  maxdelta %zu\n", diff, UVLEN, maxd);
		printf("   of those, %zu are >=5 B from a plateau boundary, %zu are within 4 B of one\n",
		       interior_diff, edge_diff);
		printf("   row 0, cols 0..15   src:");
		for (i = 0; i < 16; ++i) printf(" %02x", s[YLEN + i]);
		printf("\n   row 0, cols 0..15   dst:");
		for (i = 0; i < 16; ++i) printf(" %02x", d[YLEN + i]);
		printf("\n   row 0, cols 60..75  src:");
		for (i = 60; i < 76; ++i) printf(" %02x", s[YLEN + i]);
		printf("\n   row 0, cols 60..75  dst:");
		for (i = 60; i < 76; ++i) printf(" %02x", d[YLEN + i]);
		printf("\n   row 9, cols 0..15   src:");
		for (i = 0; i < 16; ++i) printf(" %02x", s[YLEN + 9 * W + i]);
		printf("\n   row 9, cols 0..15   dst:");
		for (i = 0; i < 16; ++i) printf(" %02x", d[YLEN + 9 * W + i]);
		printf("\n");
		sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	}

	/* ---- E: Y8 single-plane blit ----------------------------------------- */
	{
		size_t i, diff = 0, maxd = 0, low_clamped = 0;
		sync_(sf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		for (i = 0; i < YLEN; ++i) s[i] = rnd8();
		sync_(sf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		memset(d, 0xA5, NV12);
		sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		int rc = blt(sf, df, G2D_FORMAT_Y8, G2D_FORMAT_Y8, W, H);
		sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
		printf("\n== E. G2D_FORMAT_Y8 (0x30) single-plane blit, random source ==\n   rc=%d\n", rc);
		for (i = 0; i < YLEN; ++i) {
			unsigned dv;
			if (d[i] == s[i]) continue;
			++diff;
			dv = d[i] > s[i] ? d[i] - s[i] : s[i] - d[i];
			if (dv > maxd) maxd = dv;
			if (s[i] < 16 && d[i] == 16) ++low_clamped;
		}
		printf("   Y differing %zu/%zu  maxdelta %zu  (src<16 -> 16: %zu)\n",
		       diff, YLEN, maxd, low_clamped);
		/* untouched tail must still be the 0xA5 poison */
		{
			size_t tail = 0;
			for (i = YLEN; i < NV12; ++i) tail += d[i] != 0xA5;
			printf("   bytes past the Y plane modified: %zu (must be 0)\n", tail);
		}
		sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	}

	/* ---- F: ARGB8888 -> NV12, constant colours --------------------------- */
	printf("\n== F. ARGB8888 -> NV12 (vendor g2d_format_conv path), constant colour ==\n");
	{
		const unsigned char cols[3][3] = {{128, 128, 128}, {255, 0, 0}, {0, 0, 255}};
		const char *nm[3] = {"grey(128,128,128)", "red(255,0,0)", "blue(0,0,255)"};
		int k;
		for (k = 0; k < 3; ++k) {
			size_t i;
			double R = cols[k][0], G = cols[k][1], B = cols[k][2];
			double refY = 0.299 * R + 0.587 * G + 0.114 * B;
			double refU = -0.168736 * R - 0.331264 * G + 0.5 * B + 128;
			double refV = 0.5 * R - 0.418688 * G - 0.081312 * B + 128;
			sync_(af, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			for (i = 0; i + 3 < ARGB; i += 4) {
				a[i + 0] = cols[k][2]; a[i + 1] = cols[k][1];
				a[i + 2] = cols[k][0]; a[i + 3] = 0xFF;
			}
			sync_(af, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(d, 0xA5, NV12);
			sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			int rc = blt(af, df, G2D_FORMAT_ARGB8888, G2D_FORMAT_YUV420UVC_U1V1U0V0, W, H);
			sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			printf("   %-16s rc=%d  Y[0]=%3u  UV[0]=%3u UV[1]=%3u  (cpu full-range ref Y=%.0f U=%.0f V=%.0f)\n",
			       nm[k], rc, d[0], d[YLEN + 0], d[YLEN + 1], refY, refU, refV);
			sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		}
	}

	/* ---- G: NV12 -> ARGB8888, constant colour --------------------------- */
	printf("\n== G. NV12 -> ARGB8888, constant colour ==\n");
	{
		/* BT.601 full range: grey Y=128,U=128,V=128 -> R=G=B=128 */
		const unsigned char yuv[2][3] = {{128, 128, 128}, {81, 90, 240}};
		const char *nm[2] = {"Y=128 U=128 V=128", "Y=81 U=90 V=240 (red-ish)"};
		int k;
		for (k = 0; k < 2; ++k) {
			size_t i;
			double Y = yuv[k][0], U = yuv[k][1] - 128, V = yuv[k][2] - 128;
			double rr = Y + 1.402 * V, gg = Y - 0.344136 * U - 0.714136 * V, bb = Y + 1.772 * U;
			sync_(sf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(s, yuv[k][0], YLEN);
			memset(s + YLEN, yuv[k][1], UVLEN);
			for (i = 0; i < UVLEN; i += 2) s[YLEN + i + 1] = yuv[k][2];
			sync_(sf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			sync_(adf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(ad, 0xA5, ARGB);
			sync_(adf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			int rc = blt(sf, adf, G2D_FORMAT_YUV420UVC_U1V1U0V0, G2D_FORMAT_ARGB8888, W, H);
			sync_(adf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			printf("   %-22s rc=%d  pixel0 B=%3u G=%3u R=%3u A=%3u  (cpu full-range ref R=%.0f G=%.0f B=%.0f)\n",
			       nm[k], rc, ad[0], ad[1], ad[2], ad[3], rr, gg, bb);
			sync_(adf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		}
	}

	printf("\ndone\n");
	return 0;
}

/* Which 4:2:0 enum is NV12, and does the enum change how a YUV->YUV blit behaves?
 *
 * 0x28 G2D_FORMAT_YUV420UVC_V1U1V0U0
 * 0x29 G2D_FORMAT_YUV420UVC_U1V1U0V0   <-- the one used so far, "U first" by name
 *
 * H. ARGB8888 -> <fmt>: write a pure red / pure blue constant and read back
 *    UV[0] and UV[1].  For red, U=85 and V=255; for blue, U=255 and V=107
 *    (BT.601 full range), so the two values identify the byte order unambiguously.
 * I. <fmt> -> ARGB8888: feed U=90 V=240 and see which RGB comes back.
 * J. <fmt> -> <fmt> with real 1920x1080 geometry: does the enum change chroma
 *    exactness?  Two data sets: horizontally smooth (vertical-constant 64 B
 *    plateaus) and studio-range random.
 *
 * Build on the board:  gcc -O2 -o /tmp/g2d_order g2d_order.c
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

static uint32_t xs = 0x2545f491u;
static unsigned char rnd8(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return (unsigned char)(xs >> 11); }

/* fill chroma with vertically constant, horizontally smooth 64 B plateaus */
static void fill_smooth(uint8_t *s)
{
	size_t i;
	memset(s, 0x80, YLEN);
	for (i = 0; i < UVLEN; ++i)
		s[YLEN + i] = (uint8_t)(80 + ((i % W) % 1024) / 64);
}

static void fill_rnd_inrange(uint8_t *s)
{
	size_t i;
	for (i = 0; i < YLEN; ++i) s[i] = (unsigned char)(16 + rnd8() % 220);
	for (i = YLEN; i < NV12; ++i) s[i] = (unsigned char)(16 + rnd8() % 224);
}

static void copy_case(const char *label, g2d_fmt_enh f, void (*fill)(uint8_t *))
{
	uint8_t *s, *d;
	int sf = heap_alloc(NV12, (void **)&s), df = heap_alloc(NV12, (void **)&d);
	size_t i, dy = 0, duv = 0, maxuv = 0;
	if (sf < 0 || df < 0) { puts("  dma_heap failed"); return; }
	sync_(sf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW); fill(s); sync_(sf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW); memset(d, 0xA5, NV12); sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	int rc = blt(sf, df, f, f, W, H);
	sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	for (i = 0; i < YLEN; ++i) dy += d[i] != s[i];
	for (i = YLEN; i < NV12; ++i) {
		if (d[i] == s[i]) continue;
		++duv;
		unsigned dv = d[i] > s[i] ? d[i] - s[i] : s[i] - d[i];
		if (dv > maxuv) maxuv = dv;
	}
	printf("  %-22s fmt=0x%02x rc=%d  Y diff %zu/%zu  UV diff %zu/%zu maxdelta %zu\n",
	       label, f, rc, dy, YLEN, duv, UVLEN, maxuv);
	sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	munmap(s, NV12); close(sf); munmap(d, NV12); close(df);
}

int main(void)
{
	uint8_t *s, *d, *a, *ad;
	int sf, df, af, adf;
	g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	sf = heap_alloc(NV12, (void **)&s); df = heap_alloc(NV12, (void **)&d);
	af = heap_alloc(ARGB, (void **)&a); adf = heap_alloc(ARGB, (void **)&ad);
	if (sf < 0 || df < 0 || af < 0 || adf < 0) { perror("dma_heap"); return 1; }

	printf("== H. ARGB8888 -> <fmt>, constant colour (red: U=85 V=255, blue: U=255 V=107) ==\n");
	{
		const unsigned char col[2][3] = {{255, 0, 0}, {0, 0, 255}};
		const char *nm[2] = {"red ", "blue"};
		g2d_fmt_enh f;
		int k;
		for (k = 0; k < 2; ++k) {
			size_t i;
			sync_(af, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			for (i = 0; i + 3 < ARGB; i += 4) {
				a[i + 0] = col[k][2]; a[i + 1] = col[k][1];
				a[i + 2] = col[k][0]; a[i + 3] = 0xFF;
			}
			sync_(af, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			for (f = G2D_FORMAT_YUV420UVC_V1U1V0U0; f <= G2D_FORMAT_YUV420UVC_U1V1U0V0;
			     f = (g2d_fmt_enh)(f + 1)) {
				sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
				memset(d, 0xA5, NV12);
				sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
				int rc = blt(af, df, G2D_FORMAT_ARGB8888, f, W, H);
				sync_(df, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
				printf("  %s -> fmt=0x%02x rc=%d  Y=%3u  UV[0]=%3u UV[1]=%3u  => even byte is %s\n",
				       nm[k], f, rc, d[0], d[YLEN], d[YLEN + 1],
				       (d[YLEN] == 85 || d[YLEN] == 255) && d[YLEN] > d[YLEN + 1] ? "U" :
				       (d[YLEN] == 255 || d[YLEN] == 107) ? "V" : "?");
				sync_(df, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			}
		}
	}

	printf("\n== I. <fmt> -> ARGB8888, Y=81 U=90 V=240 (expect R=238 G=14 B=14 if read as U,V) ==\n");
	{
		g2d_fmt_enh f;
		size_t i;
		for (f = G2D_FORMAT_YUV420UVC_V1U1V0U0; f <= G2D_FORMAT_YUV420UVC_U1V1U0V0;
		     f = (g2d_fmt_enh)(f + 1)) {
			sync_(sf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(s, 81, YLEN); memset(s + YLEN, 90, UVLEN);
			for (i = 0; i < UVLEN; i += 2) s[YLEN + i + 1] = 240;
			sync_(sf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			sync_(adf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			memset(ad, 0xA5, ARGB);
			sync_(adf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
			int rc = blt(sf, adf, f, G2D_FORMAT_ARGB8888, W, H);
			sync_(adf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
			printf("  fmt=0x%02x rc=%d  B=%3u G=%3u R=%3u A=%3u  => looks like %s\n", f, rc,
			       ad[0], ad[1], ad[2], ad[3],
			       (ad[2] > 200 && ad[1] < 60) ? "U,V read correctly (NV12)" :
			       (ad[0] > 200 && ad[1] < 60) ? "U,V SWAPPED (treated as NV21)" : "neither");
			sync_(adf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
		}
	}

	printf("\n== J. <fmt> -> <fmt> chroma exactness, 1920x1080 ==\n");
	{
		g2d_fmt_enh f;
		for (f = G2D_FORMAT_YUV420UVC_V1U1V0U0; f <= G2D_FORMAT_YUV420UVC_U1V1U0V0;
		     f = (g2d_fmt_enh)(f + 1)) {
			copy_case("smooth chroma", f, fill_smooth);
			copy_case("in-range random", f, fill_rnd_inrange);
		}
	}

	printf("\ndone\n");
	return 0;
}

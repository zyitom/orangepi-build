/* Which source byte does each destination byte come from?
 *
 * The earlier probes used a 256-byte-period ramp, which cannot distinguish a
 * translation from a resampling and aliases badly. Instead this probe plants a
 * single isolated impulse (0xEE in a 0x80 field) at one known source offset per
 * run, poisons the destination with 0xA5 (so "was this byte written at all?" is
 * unambiguous) and reports every destination byte that changed.
 *
 *   identity copy          -> exactly one changed byte, at the same offset
 *   translation by S       -> exactly one changed byte, at offset X (+/- S)
 *   resampling / filtering -> a small *cluster* around the mapped position, with
 *                             attenuated amplitude
 *   chroma never written   -> zero changed bytes
 *
 * Build on the board (glibc 2.31):  gcc -O2 -o /tmp/g2d_mark g2d_mark.c
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

static uint8_t *src, *dst;
static int sfd, dfd;

/* put a single impulse in the source plane and report every dst byte that moved */
static void marker(const char *what, size_t plane_off, size_t x, unsigned char imp)
{
	size_t i;
	sync_(sfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	memset(src, 0x80, NV12);
	src[plane_off + x] = imp;
	sync_(sfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	sync_(dfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	memset(dst, 0xA5, NV12);
	sync_(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);

	int rc = blt(sfd, dfd, G2D_FORMAT_YUV420UVC_U1V1U0V0, G2D_FORMAT_YUV420UVC_U1V1U0V0, W, H);
	sync_(dfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	printf("%s src[%zu] (plane rel %zu) = 0x%02x  rc=%d  ->  ", what, plane_off + x, x, imp, rc);
	if (rc < 0) { printf("ioctl failed %s\n", strerror(errno)); sync_(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW); return; }
	{
		size_t changed = 0, shown = 0;
		for (i = plane_off; i < plane_off + (plane_off ? UVLEN : YLEN); ++i)
			if (dst[i] != 0xA5) ++changed;
		printf("%zu dst byte(s) written where poison was 0xA5: ", changed);
		for (i = plane_off; i < plane_off + (plane_off ? UVLEN : YLEN) && shown < 12; ++i)
			if (dst[i] != 0xA5) {
				size_t rel = i - plane_off;
				printf("[%zu]=0x%02x ", rel, dst[i]);
				++shown;
			}
		printf("\n");
	}
	sync_(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
}

/* is the plane written at all?  how many bytes equal the source? */
static void identity(const char *what, int mode)
{
	size_t i, eq = 0, changed = 0;
	sync_(sfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	if (mode == 0) {
		for (i = 0; i < NV12; ++i) src[i] = 0x80;                      /* flat */
	} else {
		for (i = 0; i < YLEN; ++i) src[i] = 0x80;
		for (i = 0; i < UVLEN; ++i) src[YLEN + i] = (uint8_t)(0x80 + ((i / 64) % 16) * 4);  /* 64-byte plateaus */
	}
	sync_(sfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	sync_(dfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	memset(dst, 0xA5, NV12);
	sync_(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	int rc = blt(sfd, dfd, G2D_FORMAT_YUV420UVC_U1V1U0V0, G2D_FORMAT_YUV420UVC_U1V1U0V0, W, H);
	sync_(dfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	printf("%s rc=%d: ", what, rc);
	for (i = YLEN; i < NV12; ++i) {
		if (dst[i] == src[i]) ++eq;
		if (dst[i] != 0xA5) ++changed;
	}
	printf("UV bytes written %zu/%zu, equal to src %zu/%zu\n", changed, UVLEN, eq, UVLEN);
	if (mode == 1) {
		printf("     plateau samples (expect src value):");
		for (i = 0; i < 8; ++i) {
			size_t k = 32 + (size_t)i * 64 * 16;
			if (k >= UVLEN) break;
			printf(" [%zu] src=0x%02x dst=0x%02x", k, src[YLEN + k], dst[YLEN + k]);
		}
		printf("\n");
	}
	sync_(dfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
}

int main(void)
{
	g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	sfd = heap_alloc(NV12, (void **)&src);
	dfd = heap_alloc(NV12, (void **)&dst);
	if (sfd < 0 || dfd < 0) { perror("dma_heap"); return 1; }
	printf("NV12 %dx%d  Y=0x%zx..0x%zx  UV=0x%zx..0x%zx\n", W, H, (size_t)0, YLEN, YLEN, NV12);

	puts("\n== A. Y plane impulse ==");
	marker("Y ", 0, 0, 0xEE);
	marker("Y ", 0, 1921, 0xEE);
	marker("Y ", 0, 1920 * 500 + 5, 0xEE);

	puts("\n== B. UV plane impulse (canonical NV12: UV row = 1920 B, 540 rows) ==");
	marker("UV", YLEN, 0, 0xEE);
	marker("UV", YLEN, 1, 0xEE);
	marker("UV", YLEN, 2, 0xEE);
	marker("UV", YLEN, 3, 0xEE);
	marker("UV", YLEN, 960, 0xEE);
	marker("UV", YLEN, 1918, 0xEE);
	marker("UV", YLEN, 1919, 0xEE);
	marker("UV", YLEN, 1920, 0xEE);
	marker("UV", YLEN, 1921, 0xEE);
	marker("UV", YLEN, 1922, 0xEE);
	marker("UV", YLEN, 3840, 0xEE);
	marker("UV", YLEN, 1920 * 269, 0xEE);
	marker("UV", YLEN, 1920 * 270, 0xEE);
	marker("UV", YLEN, UVLEN - 1, 0xEE);

	puts("\n== C. flat / slowly-varying chroma ==");
	identity("flat   ", 0);
	identity("plateau", 1);

	printf("\ndone\n");
	return 0;
}

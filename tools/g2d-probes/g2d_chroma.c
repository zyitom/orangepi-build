// What does G2D do to the NV12 CHROMA plane? Fill the whole buffer with one constant,
// copy, and look at what comes out of the Y and UV regions.
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
	if (h < 0) return -1;
	memset(&d,0,sizeof d); d.len=len; d.fd_flags=O_RDWR|O_CLOEXEC;
	if (ioctl(h, DMA_HEAP_IOCTL_ALLOC, &d) < 0) { close(h); return -1; }
	close(h);
	*map = mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_SHARED,d.fd,0);
	if (*map == MAP_FAILED) { close(d.fd); return -1; }
	return (int)d.fd;
}
static int g2d_copy(int g2d,int sfd,int dfd,unsigned fmt,unsigned w,unsigned h,unsigned flag)
{
	g2d_blt_h b; memset(&b,0,sizeof b);
	b.flag_h = flag;
	b.src_image_h.fd=sfd; b.src_image_h.format=fmt;
	b.src_image_h.width=w; b.src_image_h.height=h;
	b.src_image_h.clip_rect.w=w; b.src_image_h.clip_rect.h=h;
	b.src_image_h.bbuff=1; b.src_image_h.use_phy_addr=0;
	b.dst_image_h=b.src_image_h; b.dst_image_h.fd=dfd;
	return ioctl(g2d, G2D_CMD_BITBLT_H, &b);
}
static void uniq(const uint8_t *p, size_t n, const char *tag)
{
	int seen[256]; memset(seen,0,sizeof seen);
	for (size_t i=0;i<n;i++) seen[p[i]]=1;
	int k=0; printf("      %s distinct:", tag);
	for (int v=0;v<256;v++) if (seen[v] && k<8) { printf(" 0x%02x", v); k++; }
	int total=0; for (int v=0;v<256;v++) total+=seen[v];
	printf("   (total %d)\n", total);
}

int main(void)
{
	int g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	const unsigned W=1920,H=1080;
	size_t nv=(size_t)W*H*3/2, ypl=(size_t)W*H;
	void *s,*d;
	int sf=heap_alloc(nv,&s), df=heap_alloc(nv,&d);

	unsigned flags[3] = { G2D_BLT_NONE_H, G2D_BLT_COPYPEN, G2D_ROT_0 };
	const char *fname[3] = { "G2D_BLT_NONE_H(0)", "G2D_BLT_COPYPEN", "G2D_ROT_0" };
	unsigned consts[3] = { 0x10, 0x80, 0xF0 };

	for (int f=0; f<3; f++) {
		printf("=== flag_h = %s ===\n", fname[f]);
		for (int c=0; c<3; c++) {
			unsigned v = consts[c];
			syncbuf(sf, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
			memset(s, (int)v, nv);
			syncbuf(sf, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
			syncbuf(df, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
			memset(d, 0xAA, nv);
			syncbuf(df, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
			int rc = g2d_copy(g2d, sf, df, G2D_FORMAT_YUV420UVC_U1V1U0V0, W, H, flags[f]);
			syncbuf(df, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
			size_t dy=0, du=0;
			for (size_t i=0;i<ypl;i++)      if (((uint8_t*)d)[i] != v) dy++;
			for (size_t i=ypl;i<nv;i++)     if (((uint8_t*)d)[i] != v) du++;
			printf("   fill 0x%02x  rc=%d  Y-mismatch=%zu/%zu  UV-mismatch=%zu/%zu\n",
			       v, rc, dy, ypl, du, nv-ypl);
			if (du) {
				uniq((const uint8_t*)d,        ypl,    "Y region");
				uniq((const uint8_t*)d + ypl,  nv-ypl, "UV region");
			}
			syncbuf(df, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
		}
		printf("\n");
	}
	printf("done\n");
	return 0;
}

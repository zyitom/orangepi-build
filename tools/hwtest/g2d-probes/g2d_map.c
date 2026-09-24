// Locate the NV12 chroma mapping: src[i] = i & 0xff (self-encoding ramp).
// After a copy, dst[k] should equal (k & 0xff) if the copy is layout-identical.
// Any deviation tells us the source offset G2D actually used for that dst byte.
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
static int g2d_copy(int g2d,int sfd,int dfd,unsigned fmt,unsigned w,unsigned h)
{
	g2d_blt_h b; memset(&b,0,sizeof b);
	b.flag_h = G2D_BLT_NONE_H;
	b.src_image_h.fd=sfd; b.src_image_h.format=fmt;
	b.src_image_h.width=w; b.src_image_h.height=h;
	b.src_image_h.clip_rect.w=w; b.src_image_h.clip_rect.h=h;
	b.src_image_h.bbuff=1; b.src_image_h.use_phy_addr=0;
	b.dst_image_h=b.src_image_h; b.dst_image_h.fd=dfd;
	return ioctl(g2d, G2D_CMD_BITBLT_H, &b);
}

int main(void)
{
	int g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	const unsigned W=1920,H=1080;
	size_t nv=(size_t)W*H*3/2, ypl=(size_t)W*H;
	void *s,*d;
	int sf=heap_alloc(nv,&s), df=heap_alloc(nv,&d);

	syncbuf(sf, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	for (size_t i=0;i<nv;i++) ((uint8_t*)s)[i]=(uint8_t)(i & 0xff);
	syncbuf(sf, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	syncbuf(df, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	memset(d,0xAA,nv);
	syncbuf(df, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);

	printf("ramp copy rc=%d\n", g2d_copy(g2d,sf,df,G2D_FORMAT_YUV420UVC_U1V1U0V0,W,H));
	syncbuf(df, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);

	size_t badY=0, badUV=0;
	for (size_t i=0;i<nv;i++){
		uint8_t want=(uint8_t)(i & 0xff);
		if (((uint8_t*)d)[i]!=want){ if(i<ypl) badY++; else badUV++; }
	}
	printf("torrent check: Y bad=%zu/%zu   UV bad=%zu/%zu\n", badY, ypl, badUV, nv-ypl);

	printf("probe  offset      dst    want   delta\n");
	size_t probe[] = { 0, 1, 100, 1918, 1920, 3840,
	                   ypl+0, ypl+1, ypl+2, ypl+3,
	                   ypl+1920, ypl+1921, ypl+1922, ypl+1923,
	                   ypl+2*1920, ypl+2*1920+1,
	                   (size_t)W*H + 960, (size_t)W*H + 1920 + 960, nv-1 };
	for (unsigned i=0;i<sizeof probe/sizeof probe[0];i++){
		size_t k=probe[i];
		if (k>=nv) continue;
		uint8_t got=((uint8_t*)d)[k], want=(uint8_t)(k & 0xff);
		int delta=(int)((got-want+256)&0xff);
		printf("       %8zu  0x%02x   0x%02x   %d%s\n", k, got, want, delta,
		       (k>=ypl && delta) ? "   <-- UV" : "");
	}
	syncbuf(df, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	printf("done\n");
	return 0;
}

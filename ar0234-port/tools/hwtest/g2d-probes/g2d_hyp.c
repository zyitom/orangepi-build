// G2D hypothesis test:
//  H1 (ARGB8888): G2D applies alpha-premultiply -> with alpha=0xFF everywhere, copy must be exact.
//  H2 (NV12):     G2D clamps to studio range [16,235]/[16,240] -> with all samples in range,
//                 Y must be exact; UV tells us whether anything else happens.
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
static void cmp(const char *tag, const uint8_t *a, const uint8_t *b, size_t n,
		size_t split, const char *pl1, const char *pl2)
{
	size_t diff=0, d1=0, d2=0; int maxd=0, shown=0;
	for (size_t i=0;i<n;i++) if (a[i]!=b[i]) {
		int d = a[i]>b[i]?a[i]-b[i]:b[i]-a[i];
		if (d>maxd) maxd=d; diff++;
		if (i<split) d1++; else d2++;
		if (shown<5){ printf("      off=%zu 0x%02x -> 0x%02x\n", i, a[i], b[i]); shown++; }
	}
	printf("  [%s] differing=%zu/%zu (%.3f%%) maxdelta=%d  %s\n",
	       tag, diff, n, 100.0*diff/(double)n, maxd, diff?"MISMATCH":"BYTE-EXACT");
	if (split && split < n)
		printf("      %s=%zu  %s=%zu\n", pl1, d1, pl2, d2);
}

int main(void)
{
	int g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) { perror("open /dev/g2d"); return 1; }
	const unsigned W=1920,H=1080;

	/* ---------- H1: ARGB8888 with alpha = 0xFF ---------- */
	size_t n=(size_t)W*H*4; void *sm,*dm;
	int sfd=heap_alloc(n,&sm), dfd=heap_alloc(n,&dm);
	syncbuf(sfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	for (size_t i=0;i<n;i+=4){
		((uint8_t*)sm)[i  ]=(uint8_t)(i*31u);        /* B */
		((uint8_t*)sm)[i+1]=(uint8_t)(i*17u);        /* G */
		((uint8_t*)sm)[i+2]=(uint8_t)(i* 7u);        /* R */
		((uint8_t*)sm)[i+3]=0xFF;                    /* A  = opaque */
	}
	syncbuf(sfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	syncbuf(dfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	memset(dm,0xAA,n);
	syncbuf(dfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	printf("=== H1: ARGB8888, alpha=0xFF everywhere ===\n");
	printf("  ioctl rc=%d\n", g2d_copy(g2d,sfd,dfd,G2D_FORMAT_ARGB8888,W,H));
	syncbuf(dfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	cmp("H1 opaque-alpha copy", sm, dm, n, 0, 0, 0);
	syncbuf(dfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);

	/* ---------- H2: NV12 with all samples in [16,240] ---------- */
	size_t nv=(size_t)W*H*3/2; void *s2,*d2;
	int s2f=heap_alloc(nv,&s2), d2f=heap_alloc(nv,&d2);
	syncbuf(s2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	for (size_t i=0;i<nv;i++) ((uint8_t*)s2)[i]=(uint8_t)(16 + ((i*31u)%225u)); /* 16..240 */
	syncbuf(s2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	syncbuf(d2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	memset(d2,0xAA,nv);
	syncbuf(d2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	printf("\n=== H2: NV12, all samples in [16,240] ===\n");
	printf("  ioctl rc=%d\n", g2d_copy(g2d,s2f,d2f,G2D_FORMAT_YUV420UVC_U1V1U0V0,W,H));
	syncbuf(d2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	cmp("H2 in-range NV12 copy", s2, d2, nv, (size_t)W*H, "Y", "UV");
	syncbuf(d2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);

	/* ---------- H2b: NV12 full range, but check the CLAMP MODEL numerically ---------- */
	syncbuf(s2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	for (size_t i=0;i<nv;i++) ((uint8_t*)s2)[i]=(uint8_t)(i*31u + (i>>8)*7u);
	syncbuf(s2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	syncbuf(d2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	memset(d2,0xAA,nv);
	syncbuf(d2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	printf("\n=== H2b: NV12 full range -> does clamp model explain it? ===\n");
	printf("  ioctl rc=%d\n", g2d_copy(g2d,s2f,d2f,G2D_FORMAT_YUV420UVC_U1V1U0V0,W,H));
	syncbuf(d2f, DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW);
	{
		size_t ypl=(size_t)W*H, uvpl=nv-ypl;
		size_t y_unexp=0, uv_unexp=0, y_tot=0, uv_tot=0;
		for (size_t i=0;i<nv;i++){
			uint8_t s=((uint8_t*)s2)[i], o=((uint8_t*)d2)[i];
			uint8_t pred;
			if (i<ypl) pred = s<16?16:(s>235?235:s);          /* clamp Y 16..235 */
			else       pred = s<16?16:(s>240?240:s);          /* clamp UV 16..240 */
			if (i<ypl){ y_tot++; if (o!=pred) y_unexp++; }
			else     { uv_tot++; if (o!=pred) uv_unexp++; }
		}
		printf("  Y : actual != clamp model in %zu/%zu samples\n", y_unexp, y_tot);
		printf("  UV: actual != clamp model in %zu/%zu samples\n", uv_unexp, uv_tot);
	}
	syncbuf(d2f, DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW);
	printf("\ndone\n");
	return 0;
}

// G2D test: query version, BITBLT-copy an ISP NV12 frame (DMA-BUF fd in, fd out),
// then a best-effort 2x downscale. Run as root (or with /dev/g2d accessible).
#include <ar0234/v4l2.hpp>

#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

extern "C" {
#include "sunxi-g2d.h"
}

// /dev/dma_heap/system allocation (linux/dma-heap.h may be absent)
#include <linux/ioctl.h>
struct dma_heap_allocation_data {
	__u64 len;
	__u32 fd;
	__u32 fd_flags;
	__u64 heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
struct dma_buf_sync { __u64 flags; };
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#define DMA_BUF_SYNC_READ (1 << 2)
#define DMA_BUF_SYNC_WRITE (2 << 2)
#define DMA_BUF_SYNC_RW (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START (0 << 0)
#define DMA_BUF_SYNC_END (1 << 0)

static int alloc_heap(std::size_t len)
{
	int h = open("/dev/dma_heap/system", O_RDONLY);
	if (h < 0)
		return -1;
	dma_heap_allocation_data d{};
	d.len = len;
	if (ioctl(h, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
		close(h);
		return -1;
	}
	close(h);
	return (int)d.fd;
}

static void *mmap_fd(int fd, std::size_t len)
{
	return mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

int main()
{
	int g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) {
		printf("FAIL: open /dev/g2d: %s (modprobe g2d_sunxi?)\n", strerror(errno));
		return 1;
	}
	g2d_hardware_version ver{};
	if (ioctl(g2d, G2D_CMD_QUERY_VERSION, &ver) == 0)
		printf("g2d version 0x%llx\n", (unsigned long long)ver.g2d_version);
	else
		printf("QUERY_VERSION failed: %s\n", strerror(errno));

	const unsigned w = 1920, h = 1080;
	const std::size_t nv12 = w * h * 3 / 2;

	ar0234::CaptureConfig cc{};
	cc.size = {w, h};
	cc.fps = 30;
	ar0234::Capture cap{cc};
	if (!cap.dmabuf_exported()) {
		printf("FAIL: no dmabuf export\n");
		return 1;
	}
	auto bufs = cap.buffers();
	cap.start();
	auto fr = cap.dequeue(std::chrono::seconds(2));
	if (!fr) {
		printf("FAIL: no frame\n");
		return 1;
	}
	auto &src = bufs[fr->index()];
	/* stop streaming BEFORE using other capture buffers as G2D dst:
	 * while queued, vin keeps DMA-writing new frames into them and the
	 * copy races with live frames (this was the "5% drift") */
	cap.stop();

	/* use the second capture buffer as dst (already a DMA-BUF with mapping) */
	int dstfd = bufs[1].dmabuf.get();
	void *dst = bufs[1].data().data();

	g2d_blt_h blt{};
	blt.flag_h = G2D_BLT_NONE_H;
	blt.src_image_h.fd = src.dmabuf.get();
	blt.src_image_h.format = G2D_FORMAT_YUV420UVC_U1V1U0V0;	/* NV12 */
	blt.src_image_h.width = w;
	blt.src_image_h.height = h;
	blt.src_image_h.clip_rect.x = 0;
	blt.src_image_h.clip_rect.y = 0;
	blt.src_image_h.clip_rect.w = w;
	blt.src_image_h.clip_rect.h = h;
	blt.src_image_h.use_phy_addr = 0;
	blt.src_image_h.bbuff = 1;
	blt.dst_image_h = blt.src_image_h;
	blt.dst_image_h.fd = dstfd;

	auto dmasync = [&](int fd, unsigned flags) {
		struct dma_buf_sync { __u64 flags; } s{flags};
		ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
	};
	dmasync(dstfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
	if (ioctl(g2d, G2D_CMD_BITBLT_H, &blt) < 0) {
		printf("FAIL: BITBLT copy: %s\n", strerror(errno));
		return 1;
	}
	dmasync(dstfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
	printf("src[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x  dst[0..7]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
	       src.data()[0],src.data()[1],src.data()[2],src.data()[3],src.data()[4],src.data()[5],src.data()[6],src.data()[7],
	       ((unsigned char*)dst)[0],((unsigned char*)dst)[1],((unsigned char*)dst)[2],((unsigned char*)dst)[3],
	       ((unsigned char*)dst)[4],((unsigned char*)dst)[5],((unsigned char*)dst)[6],((unsigned char*)dst)[7]);
	int same = memcmp(dst, src.data().data(), nv12) == 0;
	std::size_t diff = 0;
	if (!same)
		for (std::size_t i = 0; i < nv12; ++i)
			diff += ((unsigned char *)dst)[i] != src.data()[i];
	printf("copy: %s (differing bytes %zu/%zu)\n", same ? "OK" : "MISMATCH", diff, nv12);
	{
		std::size_t ydiff = 0, uvdiff = 0, first = nv12;
		for (std::size_t i = 0; i < nv12; ++i) {
			if (((unsigned char *)dst)[i] != src.data()[i]) {
				++(i < w * h ? ydiff : uvdiff);
				if (first == nv12) first = i;
			}
		}
		int tol[4] = {0, 0, 0, 0};   /* 0, <=2, <=8, >8 */
		for (std::size_t i = 0; i < nv12; ++i) {
			unsigned d = ((unsigned char *)dst)[i] > src.data()[i]
					     ? ((unsigned char *)dst)[i] - src.data()[i]
					     : src.data()[i] - ((unsigned char *)dst)[i];
			++tol[d == 0 ? 0 : d <= 2 ? 1 : d <= 8 ? 2 : 3];
		}
		printf("diff magnitude: exact %zu, <=2 %d, <=8 %d, >8 %d\n",
		       nv12 - diff, tol[1], tol[2], tol[3]);
	}

	/* best-effort 2x downscale into a second buffer */
	{
		int sfd = bufs[2].dmabuf.get();
		void *sdst = bufs[2].data().data();
		g2d_blt_h sc = blt;
		sc.dst_image_h.fd = sfd;
		sc.dst_image_h.width = w / 2;
		sc.dst_image_h.height = h / 2;
		sc.dst_image_h.clip_rect.w = w / 2;
		sc.dst_image_h.clip_rect.h = h / 2;
		sc.src_image_h.resize.w = w / 2;
		sc.src_image_h.resize.h = h / 2;
		if (ioctl(g2d, G2D_CMD_BITBLT_H, &sc) < 0)
			printf("scale: FAILED: %s\n", strerror(errno));
		else {
			std::size_t nz = 0;
			auto *p = (unsigned char *)sdst;
			for (std::size_t i = 0; i < nv12 / 4; ++i)
				nz += p[i] != 0;
			printf("scale: OK (nonzero bytes %zu/%zu)\n", nz, nv12 / 4);
		}
	}

	printf("done\n");
	return 0;
}

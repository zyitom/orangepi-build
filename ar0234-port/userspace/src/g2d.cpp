// SPDX-License-Identifier: GPL-2.0-or-later
//
// Implementation of the narrow G2D wrapper. See g2d.hpp for why it is this small
// and analysis/g2d/REPORT.md for the measurements behind every constant here.

#include <ar0234/g2d.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include "sunxi-g2d.h"
}

namespace ar0234 {
namespace {

/* ---- dma-heap / dma-buf UAPI (this kernel ships neither header) ----------- */
struct dma_heap_allocation_data {
	std::uint64_t len;
	std::uint32_t fd;
	std::uint32_t fd_flags;
	std::uint64_t heap_flags;
};
constexpr unsigned long kDmaHeapIoctlAlloc =
	_IOWR('H', 0x0, struct dma_heap_allocation_data);

struct dma_buf_sync {
	std::uint64_t flags;
};
constexpr unsigned long kDmaBufIoctlSync = _IOW('b', 0, struct dma_buf_sync);

/*
 * DMA_BUF_SYNC_* flag encoding.  Two layouts exist:
 *
 *   legacy (this vendor 6.6 tree, uapi/linux/dma-buf.h):
 *       READ = 1 << 0, WRITE = 2 << 0, RW = 3, START = 0 << 2, END = 1 << 2
 *       (DMA_BUF_SYNC_VALID_FLAGS_MASK = RW | END = 7)
 *   mainline (6.12+):
 *       READ = 1 << 2, WRITE = 2 << 2, RW = 12, START = 0, END = 1
 *       (VALID_FLAGS_MASK = 13)
 *
 * The kernel rejects anything outside its own mask with EINVAL, and the two
 * layouts are distinguishable (legacy RW = 3 is outside the mainline mask,
 * mainline RW = 12 is outside the legacy mask), so the first SYNC call probes
 * both and remembers the one that worked.  This is not paranoia: userspace
 * tools here had the mainline constants hard-coded, the ioctl then failed with
 * EINVAL, the return value was ignored, and the "cache maintenance" silently
 * did nothing.
 */
struct SyncEncoding {
	std::uint64_t read, write, end;
};
constexpr SyncEncoding kLegacySync{1 << 0, 2 << 0, 1 << 2};
constexpr SyncEncoding kMainlineSync{1 << 2, 2 << 2, 1 << 0};

const SyncEncoding *g_sync_encoding = nullptr;   /* detected once per process */

std::uint64_t sync_flags(const SyncEncoding &e, bool end, bool write)
{
	/* The direction is mandatory: flags without READ or WRITE are rejected
	 * with EINVAL (measured: 0 and END-alone both fail). */
	return (end ? e.end : 0) | (write ? e.write : e.read);
}

[[noreturn]] void fail(const char *what)
{
	throw std::runtime_error{std::string{"ar0234::g2d: "} + what + ": " +
				 std::strerror(errno)};
}

// One G2D_FORMAT_Y8 blit: `fd` is viewed as `rows` rows of `width` 8-bit
// samples and the rectangle (0, y0, width, height) is copied. That is the only
// bit-exact operation on this SoC, and it is what the NV12 helpers are built
// from: the luma plane is (0, 0, w, h) and the chroma plane (0, h, w, h/2) of
// the same buffer viewed as w x (h + h/2).
void y8_blit(int g2d, int src_fd, int dst_fd, std::uint32_t width, std::uint32_t rows,
	     std::uint32_t y0, std::uint32_t height)
{
	g2d_blt_h b{};
	b.flag_h = G2D_BLT_NONE_H;          /* G2D_BLT_COPYPEN returns EINVAL here */
	b.src_image_h.fd = src_fd;
	b.src_image_h.format = G2D_FORMAT_Y8;
	b.src_image_h.width = width;
	b.src_image_h.height = rows;
	b.src_image_h.clip_rect.x = 0;
	b.src_image_h.clip_rect.y = static_cast<__s32>(y0);
	b.src_image_h.clip_rect.w = width;
	b.src_image_h.clip_rect.h = height;
	b.src_image_h.bbuff = 1;            /* bbuff = 0 reads stale data */
	b.src_image_h.use_phy_addr = 0;     /* fd path: the driver derives laddr[] */
	b.dst_image_h = b.src_image_h;
	b.dst_image_h.fd = dst_fd;

	if (::ioctl(g2d, G2D_CMD_BITBLT_H, &b) < 0)
		fail("G2D_CMD_BITBLT_H");
}

} // namespace

/* ------------------------------------------------------------------ DmaBuffer */
DmaBuffer::DmaBuffer(std::size_t bytes, const char *heap) : size_{bytes}
{
	if (bytes == 0)
		throw std::invalid_argument{"ar0234::DmaBuffer: zero bytes"};

	UniqueFd h{::open(heap, O_RDONLY | O_CLOEXEC)};
	if (!h)
		fail("open dma-heap");

	dma_heap_allocation_data d{};
	d.len = bytes;
	d.fd_flags = O_RDWR | O_CLOEXEC;
	if (::ioctl(h.get(), kDmaHeapIoctlAlloc, &d) < 0)
		fail("DMA_HEAP_IOCTL_ALLOC");

	fd_.reset(static_cast<int>(d.fd));
	void *m = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), 0);
	if (m == MAP_FAILED)
		fail("mmap dma-buf");
	map_ = static_cast<std::uint8_t *>(m);
}

void DmaBuffer::sync(bool end, bool write) const
{
	if (!g_sync_encoding) {
		/* Probe once: whichever encoding the kernel accepts is the one we use
		 * for the rest of the process. */
		for (const SyncEncoding *enc : {&kLegacySync, &kMainlineSync}) {
			dma_buf_sync s{sync_flags(*enc, end, write)};
			if (::ioctl(fd_.get(), kDmaBufIoctlSync, &s) == 0) {
				g_sync_encoding = enc;
				return;
			}
			if (errno != EINVAL)
				fail("DMA_BUF_IOCTL_SYNC");
		}
		throw std::runtime_error{"ar0234::g2d: DMA_BUF_IOCTL_SYNC rejected both "
					 "known flag encodings"};
	}

	dma_buf_sync s{sync_flags(*g_sync_encoding, end, write)};
	if (::ioctl(fd_.get(), kDmaBufIoctlSync, &s) < 0)
		fail("DMA_BUF_IOCTL_SYNC");
}

void DmaBuffer::reset() noexcept
{
	if (map_) {
		::munmap(map_, size_);
		map_ = nullptr;
	}
	fd_.reset();
	size_ = 0;
}

/* ------------------------------------------------------------------ DiffStat */
DiffStat diff_bytes(const std::uint8_t *a, const std::uint8_t *b, std::size_t bytes) noexcept
{
	DiffStat s;
	s.bytes = bytes;
	for (std::size_t i = 0; i < bytes; ++i) {
		if (a[i] == b[i])
			continue;
		++s.differ;
		const unsigned d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
		if (d > s.max_delta)
			s.max_delta = d;
	}
	return s;
}

/* ---------------------------------------------------------------------- G2d */
G2d::G2d(const std::string &node) : fd_{open_or_throw(node.c_str(), O_RDWR)} {}

void G2d::move_nv12(const DmaBuffer &src, const DmaBuffer &dst, std::uint32_t width,
		    std::uint32_t height) const
{
	const std::size_t need = nv12_size(width, height);
	if (src.size() < need || dst.size() < need)
		throw std::runtime_error{"ar0234::g2d: move_nv12: buffer smaller than " +
					 std::to_string(need) + " bytes"};
	move_nv12(src.fd(), dst.fd(), width, height);
}

void G2d::move_nv12(int src_fd, int dst_fd, std::uint32_t width, std::uint32_t height) const
{
	if (width == 0 || height == 0 || (height & 1))
		throw std::invalid_argument{"ar0234::g2d: move_nv12 needs even height and "
					    "non-zero size"};
	/* Copying a frame onto itself is defined as "unchanged"; do not make the
	 * hardware read and write the same lines for nothing. */
	if (src_fd == dst_fd)
		return;

	const std::uint32_t rows = height + height / 2;   /* the NV12 buffer is w x rows */
	y8_blit(fd_.get(), src_fd, dst_fd, width, rows, 0, height);          /* luma   */
	y8_blit(fd_.get(), src_fd, dst_fd, width, rows, height, height / 2); /* chroma */
}

void G2d::fill_rect_nv12(const DmaBuffer &dst, std::uint32_t width, std::uint32_t height,
			 const Rect &r, std::uint8_t y, std::uint8_t u, std::uint8_t v) const
{
	if (dst.size() < nv12_size(width, height))
		throw std::runtime_error{"ar0234::g2d: fill_rect_nv12: buffer too small"};
	fill_rect_nv12(dst.fd(), width, height, r, y, u, v);
}

void G2d::fill_rect_nv12(int dst_fd, std::uint32_t width, std::uint32_t height,
			 const Rect &r, std::uint8_t y, std::uint8_t u, std::uint8_t v) const
{
	if (width == 0 || height == 0 || r.w == 0 || r.h == 0)
		return;

	const std::uint32_t x0 = std::min(r.x, width);
	const std::uint32_t y0 = std::min(r.y, height);
	const std::uint32_t w = std::min(r.w, width - x0);
	const std::uint32_t h = std::min(r.h, height - y0);
	if (w == 0 || h == 0)
		return;

	g2d_fillrect_h fr{};
	fr.dst_image_h.fd = dst_fd;
	/* 0x28 = G2D_FORMAT_YUV420UVC_V1U1V0U0 -- the constant that means real
	 * V4L2 NV12 (U in the even byte). Its sibling 0x29 is NV21 despite its
	 * name. Both work here because the colour is written through verbatim,
	 * but 0x28 is the value a reader should see. */
	fr.dst_image_h.format = G2D_FORMAT_YUV420UVC_V1U1V0U0;
	fr.dst_image_h.width = width;
	fr.dst_image_h.height = height;
	fr.dst_image_h.clip_rect.x = static_cast<__s32>(x0);
	fr.dst_image_h.clip_rect.y = static_cast<__s32>(y0);
	fr.dst_image_h.clip_rect.w = w;
	fr.dst_image_h.clip_rect.h = h;
	fr.dst_image_h.bbuff = 1;
	fr.dst_image_h.use_phy_addr = 0;
	/* Written straight through as three bytes, no RGB->YUV matrix. */
	fr.dst_image_h.color = (static_cast<std::uint32_t>(y) << 16) |
			       (static_cast<std::uint32_t>(u) << 8) |
			       static_cast<std::uint32_t>(v);

	if (::ioctl(fd_.get(), G2D_CMD_FILLRECT_H, &fr) < 0)
		fail("G2D_CMD_FILLRECT_H");
}

void G2d::convert_nv12_to_bgr888(int src_fd, int dst_fd, std::uint32_t src_w,
				 std::uint32_t src_h, std::uint32_t dst_w,
				 std::uint32_t dst_h) const
{
	if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
		throw std::invalid_argument{"ar0234::g2d: convert_nv12_to_bgr888: "
					    "zero size"};

	/* Same blit engine as the NV12 -> NV12 case, but the write-back format
	 * is 3-byte BGR888 and the destination is a different size, so the VSU
	 * rescales on the way through. 0x28 = the constant that means real
	 * V4L2 NV12 (U in the even byte). */
	g2d_blt_h b{};
	b.flag_h = G2D_BLT_NONE_H;
	b.src_image_h.fd = src_fd;
	b.src_image_h.format = G2D_FORMAT_YUV420UVC_V1U1V0U0;
	b.src_image_h.width = src_w;
	b.src_image_h.height = src_h;
	b.src_image_h.clip_rect.x = 0;
	b.src_image_h.clip_rect.y = 0;
	b.src_image_h.clip_rect.w = src_w;
	b.src_image_h.clip_rect.h = src_h;
	b.src_image_h.bbuff = 1;
	b.src_image_h.use_phy_addr = 0;
	b.dst_image_h = b.src_image_h;
	b.dst_image_h.fd = dst_fd;
	b.dst_image_h.format = G2D_FORMAT_BGR888;
	b.dst_image_h.width = dst_w;
	b.dst_image_h.height = dst_h;
	b.dst_image_h.clip_rect.w = dst_w;
	b.dst_image_h.clip_rect.h = dst_h;

	if (::ioctl(fd_.get(), G2D_CMD_BITBLT_H, &b) < 0)
		fail("G2D_CMD_BITBLT_H (nv12 -> bgr888)");
}

} // namespace ar0234

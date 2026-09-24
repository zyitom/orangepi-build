// SPDX-License-Identifier: GPL-2.0-or-later
//
// Small, deliberately narrow G2D wrapper for the A733 (sun60iw2).
//
// WHY IT EXISTS
//   /dev/g2d is how we move and annotate the ISP's NV12 frames without the CPU,
//   but only two of the many things the driver offers have been *measured* to be
//   correct on this SoC (full evidence: analysis/g2d/REPORT.md):
//
//     * bit-exact NV12 moves -- done as TWO G2D_FORMAT_Y8 blits over the same
//       buffer viewed as (width) x (height + height/2) rows of 8-bit data, one
//       clip rectangle for the luma plane and one for the chroma plane. A single
//       4:2:0 (G2D_FORMAT_YUV420UVC_*) blit is NOT a byte mover: the luma is
//       floored at 16 and the chroma goes through the mixer's 4:2:0 resampler,
//       so it can never be bit-exact. Measured on random data and on real ISP
//       frames the Y8 pair is 0/3110400 differing bytes.
//
//     * rectangle annotation on NV12 -- G2D_CMD_FILLRECT_H. The colour is
//       dst_image_h.color written straight through as (Y<<16)|(U<<8)|V with no
//       RGB->YUV matrix, and the hardware halves the rectangle for the chroma
//       plane by itself.
//
//   Everything else is intentionally not exposed. In particular this header has
//   no way to express a 4:2:0 blit, because "NV12 in -> NV12 out" through the
//   mixer is not an identity operation and a caller could not tell from the API.
//
// CPU-SIDE SYNC DISCIPLINE
//   before the CPU writes a buffer:  cpu_write_begin(); ...write...; cpu_write_end();
//   after G2D wrote a buffer, before the CPU reads it:
//                                    cpu_read_begin();  ...read...;  cpu_read_end();
//   i.e. the DMA_BUF_IOCTL_SYNC pair must *bracket* the CPU access, not the
//   ioctl, and the first access after a blit is always a fresh pair.
//
//   The flag encoding of DMA_BUF_SYNC_* is NOT the mainline one on this vendor
//   kernel: uapi/linux/dma-buf.h here still has READ=1<<0, WRITE=2<<0, END=1<<2
//   (mainline moved READ/WRITE to bits 2/3 and END to bit 0). Getting that
//   wrong makes the ioctl fail with EINVAL and, if the caller ignores the
//   return value, silently turns cache maintenance off -- which is exactly what
//   had happened in tools/g2d_test.cpp. This wrapper therefore *negotiates* the
//   encoding by trying both once and remembering which the kernel accepts; see
//   sync() in g2d.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <ar0234/unique_fd.hpp>

namespace ar0234 {

/// Rectangle in pixels, top-left origin, used for the annotation helper.
struct Rect {
	std::uint32_t x = 0;
	std::uint32_t y = 0;
	std::uint32_t w = 0;
	std::uint32_t h = 0;
};

/// A buffer from /dev/dma_heap/system. This is the buffer flavour that both G2D
/// (fd path, use_phy_addr = 0) and the ISP/VE (DMA-BUF import) accept, so a
/// capture buffer can be moved or annotated without a CPU copy.
class DmaBuffer {
public:
	DmaBuffer() = default;
	/// Allocate `bytes` from a dma-heap, RW, and mmap it. Throws.
	/// `heap` selects the device: "/dev/dma_heap/system" (default) or
	/// "/dev/dma_heap/reserved" (CMA-backed, physically contiguous -- the one
	/// to use for hardware that cannot page). Throws.
	explicit DmaBuffer(std::size_t bytes,
			   const char *heap = "/dev/dma_heap/system");
	~DmaBuffer() { reset(); }

	DmaBuffer(const DmaBuffer &) = delete;
	DmaBuffer &operator=(const DmaBuffer &) = delete;
	DmaBuffer(DmaBuffer &&other) noexcept
		: fd_{std::move(other.fd_)}, size_{std::exchange(other.size_, 0)},
		  map_{std::exchange(other.map_, nullptr)}
	{
	}
	DmaBuffer &operator=(DmaBuffer &&other) noexcept
	{
		if (this != &other) {
			reset();
			fd_ = std::move(other.fd_);
			size_ = std::exchange(other.size_, 0);
			map_ = std::exchange(other.map_, nullptr);
		}
		return *this;
	}

	[[nodiscard]] int fd() const noexcept { return fd_.get(); }
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	[[nodiscard]] std::uint8_t *data() noexcept { return map_; }
	[[nodiscard]] const std::uint8_t *data() const noexcept { return map_; }

	/// CPU -> device: begin() before writing, end() flushes to the device.
	void cpu_write_begin() const { sync(false, true); }
	void cpu_write_end() const { sync(true, true); }
	/// Device -> CPU: begin() before reading anything G2D wrote, end() after.
	void cpu_read_begin() const { sync(false, false); }
	void cpu_read_end() const { sync(true, false); }

private:
	/// `end` = finishing a CPU access session, `write` = the CPU wrote the buffer.
	void sync(bool end, bool write) const;
	void reset() noexcept;

	UniqueFd fd_;
	std::size_t size_ = 0;
	std::uint8_t *map_ = nullptr;
};

/// Byte-for-byte comparison of two planes: the acceptance check for "bit-exact".
struct DiffStat {
	std::size_t bytes = 0;      ///< bytes compared
	std::size_t differ = 0;     ///< bytes that differ
	std::size_t max_delta = 0;  ///< largest absolute difference seen
	[[nodiscard]] bool exact() const noexcept { return differ == 0; }
};

[[nodiscard]] DiffStat diff_bytes(const std::uint8_t *a, const std::uint8_t *b,
				  std::size_t bytes) noexcept;

/// The two G2D operations that are safe on this SoC. Constructing opens
/// /dev/g2d, which needs the `video` group (or root).
class G2d {
public:
	explicit G2d(const std::string &node = "/dev/g2d");
	~G2d() = default;

	G2d(const G2d &) = delete;
	G2d &operator=(const G2d &) = delete;

	[[nodiscard]] int fd() const noexcept { return fd_.get(); }

	/// Bytes a width x height NV12 frame occupies.
	[[nodiscard]] static constexpr std::size_t nv12_size(std::uint32_t width,
							    std::uint32_t height) noexcept
	{
		return static_cast<std::size_t>(width) * height * 3 / 2;
	}

	/// Bit-exact NV12 move of a width x height frame. Both buffers may be the
	/// same one: a frame copied onto itself is by definition unchanged, so that
	/// case returns without touching the hardware. `width` is the buffer pitch
	/// -- there is no stride/padding support (align[] stays 0, every plane pitch
	/// is linear).
	void move_nv12(const DmaBuffer &src, const DmaBuffer &dst,
		       std::uint32_t width, std::uint32_t height) const;

	/// Same, for raw dma-buf fds (e.g. an exported V4L2 capture buffer). The
	/// caller guarantees both buffers hold width * height * 3 / 2 bytes.
	void move_nv12(int src_fd, int dst_fd, std::uint32_t width, std::uint32_t height) const;

	/// Fill `r` with the NV12 colour (y, u, v) verbatim -- no RGB conversion.
	/// The rectangle is clipped to the frame; for the chroma plane the hardware
	/// uses the enclosing pair (x/2, y/2, w/2, h/2) by itself.
	void fill_rect_nv12(const DmaBuffer &dst, std::uint32_t width, std::uint32_t height,
			    const Rect &r, std::uint8_t y, std::uint8_t u, std::uint8_t v) const;
	void fill_rect_nv12(int dst_fd, std::uint32_t width, std::uint32_t height,
			    const Rect &r, std::uint8_t y, std::uint8_t u, std::uint8_t v) const;

	/// One YUV -> RGB blit with rescale in the same pass: NV12 `src_fd`
	/// (src_w x src_h) -> packed BGR888 `dst_fd` (dst_w x dst_h). This goes
	/// through the mixer's resampler and colour converter, so like every
	/// 4:2:0 path it is NOT bit-exact -- but unlike the NV12 -> NV12 case the
	/// conversion is the point, and an inference engine only needs it to be
	/// *consistent* from frame to frame, which the hardware path is.
	/// Measured as the ISP -> NPU feeding path by apps/ar0234-npu-zerocopy.cpp.
	/// Note the 3-byte channel order follows the hardware's BGR888 definition;
	/// verify it against the model's expectation before reading semantics
	/// (class names) out of an inference result.
	void convert_nv12_to_bgr888(int src_fd, int dst_fd, std::uint32_t src_w,
				    std::uint32_t src_h, std::uint32_t dst_w,
				    std::uint32_t dst_h) const;

private:
	UniqueFd fd_;
};

} // namespace ar0234

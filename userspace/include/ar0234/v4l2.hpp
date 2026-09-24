// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <linux/videodev2.h>

#include "ar0234/unique_fd.hpp"

namespace ar0234 {

/// sunxi-vin private V4L2 events (bsp/drivers/vin/vin_test/sunxi_camera_v2.h)
inline constexpr std::uint32_t kEventVinClass = V4L2_EVENT_PRIVATE_START | 0x100;
inline constexpr std::uint32_t kEventVinIspOff = kEventVinClass | 0x3;

/// Resolve a V4L2 device node by its sysfs name, e.g. "sunxi_isp.0" -> /dev/v4l-subdev12.
[[nodiscard]] std::optional<std::filesystem::path> find_v4l2_node(std::string_view name);

struct Resolution {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	friend bool operator==(const Resolution &, const Resolution &) = default;
};

/// A V4L2 sub-device used to watch events and query the active pad format.
class Subdev {
public:
	explicit Subdev(const std::filesystem::path &node);

	void subscribe(std::initializer_list<std::uint32_t> event_types);
	/// Waits up to `timeout` for an event; nullopt on timeout.
	[[nodiscard]] std::optional<v4l2_event> next_event(std::chrono::milliseconds timeout);
	[[nodiscard]] std::optional<Resolution> pad_format(std::uint32_t pad = 0) const;
	/// VIDIOC_S_CTRL on the sub-device (e.g. the sensor's test_pattern).
	void set_control(std::uint32_t id, std::int32_t value);
	/// VIDIOC_G_CTRL on the sub-device; throws on error.
	[[nodiscard]] std::int32_t get_control(std::uint32_t id) const;
	[[nodiscard]] int fd() const noexcept { return fd_.get(); }

private:
	UniqueFd fd_;
};

[[nodiscard]] inline std::chrono::nanoseconds event_time(const v4l2_event &ev) noexcept
{
	return std::chrono::seconds{ev.timestamp.tv_sec} + std::chrono::nanoseconds{ev.timestamp.tv_nsec};
}

/// Move-only owner of an mmap() region.
class MappedMemory {
public:
	MappedMemory() noexcept = default;
	MappedMemory(void *addr, std::size_t length) noexcept : span_{static_cast<std::uint8_t *>(addr), length} {}
	MappedMemory(const MappedMemory &) = delete;
	MappedMemory &operator=(const MappedMemory &) = delete;
	MappedMemory(MappedMemory &&other) noexcept : span_{std::exchange(other.span_, {})} {}
	MappedMemory &operator=(MappedMemory &&other) noexcept
	{
		if (this != &other) {
			reset();
			span_ = std::exchange(other.span_, {});
		}
		return *this;
	}
	~MappedMemory() { reset(); }

	[[nodiscard]] std::span<std::uint8_t> span() const noexcept { return span_; }

private:
	void reset() noexcept;

	std::span<std::uint8_t> span_;
};

/// One mmap'ed, DMA-BUF exported NV12 capture buffer.
struct CaptureBuffer {
	MappedMemory memory;
	UniqueFd dmabuf;

	[[nodiscard]] std::span<std::uint8_t> data() const noexcept { return memory.span(); }
};

struct CaptureConfig {
	const char *device = "/dev/video0";
	Resolution size{1920, 1080};
	int fps = 30;
	unsigned buffer_count = 4;

	// --- stream-start watchdog -------------------------------------------
	// Roughly one STREAMON in ten on this board delivers a handful of frames
	// (usually 4, i.e. one full buffer queue) and then stalls for ever: the
	// vin frame counters reset to 0, dmesg stays completely silent, and the
	// very next start works. start() therefore treats "did not reach
	// `probe_frames` frames, or any gap longer than the stall budget" as a
	// failed start and restarts the stream, so a caller never sees the dead
	// stream. Set probe_timeout_ms = 0 to disable the watchdog entirely
	// (needed for external-trigger/free-running capture below ~1 fps).
	int probe_timeout_ms = 1500;  ///< budget for the *first* frame
	unsigned probe_frames = 8;    ///< frames that prove the stream is alive
	unsigned retries = 2;         ///< restart attempts before giving up
	int stall_timeout_ms = 0;     ///< 0 = auto (4 frame periods, min 250 ms)
};

/// What the stream-start watchdog had to do. Counters live for the lifetime of
/// one Capture, so a caller can report how much of the 10% bad-start rate was
/// absorbed by retrying instead of failing.
struct StreamStats {
	unsigned start_calls = 0;       ///< start() calls that did something
	unsigned failed_probes = 0;     ///< probes that saw a dead/stalled stream
	unsigned streamon_failures = 0; ///< STREAMON rejected during a restart
	unsigned hard_reopens = 0;      ///< full close/open/reconfigure
	unsigned retries_used = 0;      ///< restarts needed by the last start()
};

class Capture;

/// A dequeued frame; returns the buffer to the driver when destroyed.
class Frame {
public:
	Frame(const Frame &) = delete;
	Frame &operator=(const Frame &) = delete;
	Frame(Frame &&other) noexcept;
	Frame &operator=(Frame &&) = delete;
	~Frame();

	[[nodiscard]] unsigned index() const noexcept { return buf_.index; }
	[[nodiscard]] std::int64_t pts_us() const noexcept
	{
		return std::int64_t{buf_.timestamp.tv_sec} * 1'000'000 + buf_.timestamp.tv_usec;
	}
	[[nodiscard]] std::uint32_t sequence() const noexcept { return buf_.sequence; }

private:
	friend class Capture;
	Frame(Capture &owner, const v4l2_buffer &buf) noexcept : owner_{&owner}, buf_{buf} {}

	Capture *owner_;
	v4l2_buffer buf_;
};

/// NV12 multi-planar capture from a sunxi-vin video node (S_INPUT, S_PARM, S_FMT, MMAP).
/// Frames must not outlive the Capture they came from.
class Capture {
public:
	explicit Capture(const CaptureConfig &cfg);
	Capture(const Capture &) = delete;
	Capture &operator=(const Capture &) = delete;
	~Capture();

	/// STREAMON plus the stream-start watchdog (see CaptureConfig): returns only
	/// once the stream has actually produced frames. Throws std::runtime_error
	/// if it still has not after cfg.retries restarts.
	void start();
	void stop() noexcept;
	/// nullopt on timeout; throws on driver errors.
	[[nodiscard]] std::optional<Frame> dequeue(std::chrono::milliseconds timeout);

	[[nodiscard]] Resolution size() const noexcept { return size_; }
	[[nodiscard]] std::span<CaptureBuffer> buffers() noexcept { return buffers_; }
	[[nodiscard]] bool dmabuf_exported() const noexcept;
	[[nodiscard]] const StreamStats &stream_stats() const noexcept { return stats_; }
	/// Effective stall budget used by the watchdog (0 when disabled).
	[[nodiscard]] std::chrono::milliseconds stall_timeout() const noexcept;

	/// VIDIOC_S_CTRL on the video node. With the ISP in use, exposure/gain
	/// controls go to libisp, which only notices changes made while it runs.
	void set_control(std::uint32_t id, std::int32_t value);
	/// VIDIOC_G_CTRL on the video node; throws on error.
	[[nodiscard]] std::int32_t get_control(std::uint32_t id) const;

private:
	friend class Frame;
	void requeue(v4l2_buffer buf) noexcept;

	void configure();     ///< S_INPUT/S_PARM/S_FMT/REQBUFS/mmap/EXPBUF/QBUF
	void release_buffers() noexcept;
	void restart_hard();
	int queue_buffer(unsigned index) noexcept;
	void stream_on();
	bool probe_stream();

	CaptureConfig cfg_;
	std::string device_;
	UniqueFd fd_;
	Resolution size_;
	std::vector<CaptureBuffer> buffers_;
	bool streaming_ = false;
	unsigned outstanding_ = 0; ///< frames handed to the caller right now
	StreamStats stats_;
};

} // namespace ar0234

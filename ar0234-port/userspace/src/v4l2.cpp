// SPDX-License-Identifier: GPL-2.0-or-later
#include "ar0234/v4l2.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include <fcntl.h>

#include <poll.h>
#include <sys/mman.h>

#include <linux/v4l2-subdev.h>

namespace ar0234 {

namespace fs = std::filesystem;

// sunxi_camera_v2.h: VIDIOC_S_PARM capture mode
constexpr std::uint32_t kModeVideo = 0x0002;

constexpr int kBufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

namespace {

void fill_buffer(v4l2_buffer &buf, v4l2_plane *planes, unsigned index)
{
	buf.index = index;
	buf.type = kBufferType;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = planes;
	buf.length = 1;
}

} // namespace

void MappedMemory::reset() noexcept
{
	if (!span_.empty())
		::munmap(span_.data(), span_.size());
	span_ = {};
}

std::optional<fs::path> find_v4l2_node(std::string_view name)
{
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator{"/sys/class/video4linux", ec}) {
		std::ifstream in{entry.path() / "name"};
		std::string node_name;
		if (std::getline(in, node_name) && node_name == name)
			return fs::path{"/dev"} / entry.path().filename();
	}
	return std::nullopt;
}

Subdev::Subdev(const fs::path &node) : fd_{open_or_throw(node.c_str(), O_RDWR)} {}

void Subdev::subscribe(std::initializer_list<std::uint32_t> event_types)
{
	for (auto type : event_types) {
		v4l2_event_subscription sub{};
		sub.type = type;
		ioctl_or_throw(fd_.get(), VIDIOC_SUBSCRIBE_EVENT, &sub, "VIDIOC_SUBSCRIBE_EVENT");
	}
}

std::optional<v4l2_event> Subdev::next_event(std::chrono::milliseconds timeout)
{
	pollfd pfd{fd_.get(), POLLPRI, 0};
	int r = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
	if (r < 0 && errno != EINTR)
		throw errno_error("poll subdev");
	if (r <= 0)
		return std::nullopt;

	v4l2_event ev{};
	if (xioctl(fd_.get(), VIDIOC_DQEVENT, &ev) < 0)
		return std::nullopt;
	return ev;
}

std::optional<Resolution> Subdev::pad_format(std::uint32_t pad) const
{
	v4l2_subdev_format fmt{};
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad = pad;
	if (xioctl(fd_.get(), VIDIOC_SUBDEV_G_FMT, &fmt) < 0)
		return std::nullopt;
	return Resolution{fmt.format.width, fmt.format.height};
}

void Subdev::set_control(std::uint32_t id, std::int32_t value)
{
	v4l2_control ctrl{id, value};
	ioctl_or_throw(fd_.get(), VIDIOC_S_CTRL, &ctrl, "VIDIOC_S_CTRL (subdev)");
}

std::int32_t Subdev::get_control(std::uint32_t id) const
{
	v4l2_control ctrl{};
	ctrl.id = id;
	ioctl_or_throw(fd_.get(), VIDIOC_G_CTRL, &ctrl, "VIDIOC_G_CTRL (subdev)");
	return ctrl.value;
}

Frame::Frame(Frame &&other) noexcept : owner_{std::exchange(other.owner_, nullptr)}, buf_{other.buf_} {}

Frame::~Frame()
{
	if (owner_)
		owner_->requeue(buf_);
}

Capture::Capture(const CaptureConfig &cfg) : cfg_{cfg}, device_{cfg.device}
{
	fd_ = open_or_throw(device_.c_str(), O_RDWR | O_NONBLOCK);
	configure();
}

// buffers_ (mappings, DMA-BUFs) are released before fd_ closes
Capture::~Capture()
{
	stop();
	release_buffers();
}

// S_INPUT binds the sensor pipeline; sunxi-vin only does that on this ioctl.
void Capture::configure()
{
	const int fd = fd_.get();

	v4l2_input input{};
	ioctl_or_throw(fd, VIDIOC_S_INPUT, &input, "VIDIOC_S_INPUT");

	v4l2_streamparm parm{};
	parm.type = kBufferType;
	parm.parm.capture.timeperframe = {1, static_cast<std::uint32_t>(cfg_.fps)};
	parm.parm.capture.capturemode = kModeVideo;
	ioctl_or_throw(fd, VIDIOC_S_PARM, &parm, "VIDIOC_S_PARM");

	v4l2_format fmt{};
	fmt.type = kBufferType;
	fmt.fmt.pix_mp.width = cfg_.size.width;
	fmt.fmt.pix_mp.height = cfg_.size.height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	ioctl_or_throw(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");
	ioctl_or_throw(fd, VIDIOC_G_FMT, &fmt, "VIDIOC_G_FMT");
	size_ = {fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height};

	v4l2_requestbuffers req{};
	req.count = cfg_.buffer_count;
	req.type = kBufferType;
	req.memory = V4L2_MEMORY_MMAP;
	ioctl_or_throw(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");

	buffers_.reserve(req.count);
	for (unsigned i = 0; i < req.count; ++i) {
		v4l2_plane planes[VIDEO_MAX_PLANES]{};
		v4l2_buffer buf{};
		fill_buffer(buf, planes, i);
		ioctl_or_throw(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");

		void *mem = ::mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				   planes[0].m.mem_offset);
		if (mem == MAP_FAILED)
			throw errno_error("mmap capture buffer");
		MappedMemory memory{mem, planes[0].length};

		v4l2_exportbuffer exp{};
		exp.type = kBufferType;
		exp.index = i;
		exp.flags = O_CLOEXEC;
		UniqueFd dmabuf{xioctl(fd, VIDIOC_EXPBUF, &exp) < 0 ? -1 : exp.fd};

		buffers_.push_back({std::move(memory), std::move(dmabuf)});
		ioctl_or_throw(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
	}
}

void Capture::release_buffers() noexcept
{
	if (buffers_.empty())
		return;
	v4l2_requestbuffers req{};
	req.count = 0;
	req.type = kBufferType;
	req.memory = V4L2_MEMORY_MMAP;
	xioctl(fd_.get(), VIDIOC_REQBUFS, &req);
	buffers_.clear(); // munmap + close the DMA-BUFs
}

/// Close and reopen the node, then reconfigure everything: the software
/// equivalent of rerunning the program, which is the only recovery known to
/// bring a stalled vin/ISP pipeline back.
void Capture::restart_hard()
{
	stop();
	release_buffers();
	fd_.reset();
	fd_ = open_or_throw(device_.c_str(), O_RDWR | O_NONBLOCK);
	configure();
}

// A buffer may be queued before STREAMON (that is how a stream starts) and
// after STREAMOFF (it will be used by the next one), so queueing does not
// depend on the streaming flag. -EINVAL means "already queued": harmless.
int Capture::queue_buffer(unsigned index) noexcept
{
	v4l2_plane planes[VIDEO_MAX_PLANES]{};
	v4l2_buffer buf{};
	fill_buffer(buf, planes, index);
	return xioctl(fd_.get(), VIDIOC_QBUF, &buf) < 0 ? -1 : 0;
}

void Capture::stream_on()
{
	int type = kBufferType;
	ioctl_or_throw(fd_.get(), VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
	streaming_ = true;
}

bool Capture::dmabuf_exported() const noexcept
{
	for (const auto &b : buffers_)
		if (!b.dmabuf)
			return false;
	return !buffers_.empty();
}

void Capture::set_control(std::uint32_t id, std::int32_t value)
{
	v4l2_control ctrl{id, value};
	ioctl_or_throw(fd_.get(), VIDIOC_S_CTRL, &ctrl, "VIDIOC_S_CTRL");
}

std::int32_t Capture::get_control(std::uint32_t id) const
{
	v4l2_control ctrl{};
	ctrl.id = id;
	ioctl_or_throw(fd_.get(), VIDIOC_G_CTRL, &ctrl, "VIDIOC_G_CTRL");
	return ctrl.value;
}

std::chrono::milliseconds Capture::stall_timeout() const noexcept
{
	if (cfg_.stall_timeout_ms > 0)
		return std::chrono::milliseconds{cfg_.stall_timeout_ms};
	if (cfg_.fps <= 0)
		return std::chrono::milliseconds{250};
	// four frame periods, but never below 250 ms: a healthy stream is never
	// anywhere near a quarter second of silence
	return std::chrono::milliseconds{std::max(4 * 1000 / cfg_.fps, 250)};
}

/// Wait until the stream has produced cfg.probe_frames frames. The first frame
/// gets its own, longer budget (the ISP pipeline takes a moment to come up);
/// every later frame must arrive within the stall budget.
bool Capture::probe_stream()
{
	unsigned seen = 0;
	while (seen < cfg_.probe_frames) {
		const auto budget = seen == 0 ? std::chrono::milliseconds{cfg_.probe_timeout_ms}
					      : stall_timeout();
		auto frame = dequeue(budget);
		if (!frame)
			return false;
		frame.reset(); // puts the buffer straight back into the queue
		++seen;
	}
	return true;
}

void Capture::start()
{
	if (streaming_)
		return;

	++stats_.start_calls;
	stats_.retries_used = 0;

	std::string reason;
	for (unsigned attempt = 0; attempt <= cfg_.retries; ++attempt) {
		if (attempt > 0) {
			// Measured on this board: after a stalled start the pipeline is
			// dead as far as the file descriptor is concerned -- a plain
			// STREAMOFF + STREAMON comes back -EINVAL. The only recovery that
			// is known to work (it is what rerunning the program does) is a
			// fresh open, so escalate to that.
			stats_.retries_used = attempt;
			stats_.hard_reopens++;
			std::fprintf(stderr, "ar0234: stream watchdog: restart %u/%u, reopening %s\n", attempt,
			    cfg_.retries, device_.c_str());
			try {
				restart_hard();
			} catch (const std::exception &e) {
				reason = e.what();
				continue;
			}
		}

		try {
			stream_on();
		} catch (const std::exception &e) {
			// some attempts fail here instead of at the probe
			stats_.streamon_failures++;
			reason = e.what();
			continue;
		}
		if (cfg_.probe_timeout_ms <= 0)
			return;
		if (probe_stream())
			return;

		// dead stream: no frames at all, or a gap far longer than a frame
		stats_.failed_probes++;
		reason = "stream stalled (no frames)";
		stop();
	}

	throw std::runtime_error{"ar0234: stream start watchdog: " + device_ + " gave no frames in " +
				 std::to_string(cfg_.retries + 1) + " attempts (" + reason + ")"};
}

void Capture::stop() noexcept
{
	if (!std::exchange(streaming_, false))
		return;
	int type = kBufferType;
	xioctl(fd_.get(), VIDIOC_STREAMOFF, &type);
}

std::optional<Frame> Capture::dequeue(std::chrono::milliseconds timeout)
{
	pollfd pfd{fd_.get(), POLLIN, 0};
	int r = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
	if (r < 0 && errno != EINTR)
		throw errno_error("poll capture");
	if (r <= 0)
		return std::nullopt;

	v4l2_plane planes[VIDEO_MAX_PLANES]{};
	v4l2_buffer buf{};
	fill_buffer(buf, planes, 0);
	if (xioctl(fd_.get(), VIDIOC_DQBUF, &buf) < 0) {
		if (errno == EAGAIN)
			return std::nullopt;
		throw errno_error("VIDIOC_DQBUF");
	}
	buf.m.planes = nullptr; // points into this stack frame
	++outstanding_;
	return Frame{*this, buf};
}

void Capture::requeue(v4l2_buffer buf) noexcept
{
	if (outstanding_ > 0)
		--outstanding_;
	if (buffers_.empty())
		return;
	queue_buffer(buf.index);
}

} // namespace ar0234

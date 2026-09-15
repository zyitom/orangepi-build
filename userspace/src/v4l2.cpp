// SPDX-License-Identifier: GPL-2.0-or-later
#include "ar0234/v4l2.hpp"

#include <fstream>
#include <string>

#include <poll.h>
#include <sys/mman.h>

#include <linux/v4l2-subdev.h>

namespace ar0234 {

namespace fs = std::filesystem;

// sunxi_camera_v2.h: VIDIOC_S_PARM capture mode
constexpr std::uint32_t kModeVideo = 0x0002;

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

Frame::Frame(Frame &&other) noexcept : owner_{std::exchange(other.owner_, nullptr)}, buf_{other.buf_} {}

Frame::~Frame()
{
	if (owner_)
		owner_->requeue(buf_);
}

Capture::Capture(const CaptureConfig &cfg) : fd_{open_or_throw(cfg.device, O_RDWR | O_NONBLOCK)}
{
	const int fd = fd_.get();

	// sunxi-vin only binds the sensor pipeline on S_INPUT
	v4l2_input input{};
	ioctl_or_throw(fd, VIDIOC_S_INPUT, &input, "VIDIOC_S_INPUT");

	v4l2_streamparm parm{};
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	parm.parm.capture.timeperframe = {1, static_cast<std::uint32_t>(cfg.fps)};
	parm.parm.capture.capturemode = kModeVideo;
	ioctl_or_throw(fd, VIDIOC_S_PARM, &parm, "VIDIOC_S_PARM");

	v4l2_format fmt{};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = cfg.size.width;
	fmt.fmt.pix_mp.height = cfg.size.height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	ioctl_or_throw(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");
	ioctl_or_throw(fd, VIDIOC_G_FMT, &fmt, "VIDIOC_G_FMT");
	size_ = {fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height};

	v4l2_requestbuffers req{};
	req.count = cfg.buffer_count;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	ioctl_or_throw(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");

	buffers_.reserve(req.count);
	for (unsigned i = 0; i < req.count; ++i) {
		v4l2_plane planes[VIDEO_MAX_PLANES]{};
		v4l2_buffer buf{};
		buf.index = i;
		buf.type = req.type;
		buf.memory = req.memory;
		buf.m.planes = planes;
		buf.length = 1;
		ioctl_or_throw(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");

		void *mem = ::mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				   planes[0].m.mem_offset);
		if (mem == MAP_FAILED)
			throw errno_error("mmap capture buffer");
		MappedMemory memory{mem, planes[0].length};

		v4l2_exportbuffer exp{};
		exp.type = req.type;
		exp.index = i;
		exp.flags = O_CLOEXEC;
		UniqueFd dmabuf{xioctl(fd, VIDIOC_EXPBUF, &exp) < 0 ? -1 : exp.fd};

		buffers_.push_back({std::move(memory), std::move(dmabuf)});
		ioctl_or_throw(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF");
	}
}

// buffers_ (mappings, DMA-BUFs) are released before fd_ closes
Capture::~Capture()
{
	stop();
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

void Capture::start()
{
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl_or_throw(fd_.get(), VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
	streaming_ = true;
}

void Capture::stop() noexcept
{
	if (!std::exchange(streaming_, false))
		return;
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
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
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = planes;
	buf.length = 1;
	if (xioctl(fd_.get(), VIDIOC_DQBUF, &buf) < 0) {
		if (errno == EAGAIN)
			return std::nullopt;
		throw errno_error("VIDIOC_DQBUF");
	}
	buf.m.planes = nullptr; // points into this stack frame
	return Frame{*this, buf};
}

void Capture::requeue(v4l2_buffer buf) noexcept
{
	if (!streaming_)
		return;
	v4l2_plane planes[VIDEO_MAX_PLANES]{};
	buf.m.planes = planes;
	buf.length = 1;
	xioctl(fd_.get(), VIDIOC_QBUF, &buf);
}

} // namespace ar0234

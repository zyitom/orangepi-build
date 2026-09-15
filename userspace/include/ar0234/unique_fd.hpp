// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ar0234 {

/// Move-only owner of a file descriptor.
class UniqueFd {
public:
	UniqueFd() noexcept = default;
	explicit UniqueFd(int fd) noexcept : fd_{fd} {}
	UniqueFd(const UniqueFd &) = delete;
	UniqueFd &operator=(const UniqueFd &) = delete;
	UniqueFd(UniqueFd &&other) noexcept : fd_{other.release()} {}
	UniqueFd &operator=(UniqueFd &&other) noexcept
	{
		reset(other.release());
		return *this;
	}
	~UniqueFd() { reset(); }

	[[nodiscard]] int get() const noexcept { return fd_; }
	[[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

	int release() noexcept { return std::exchange(fd_, -1); }
	void reset(int fd = -1) noexcept
	{
		if (fd_ >= 0)
			::close(fd_);
		fd_ = fd;
	}

private:
	int fd_ = -1;
};

[[nodiscard]] inline std::system_error errno_error(std::string_view what)
{
	return {errno, std::generic_category(), std::string{what}};
}

[[nodiscard]] inline UniqueFd open_or_throw(const char *path, int flags)
{
	UniqueFd fd{::open(path, flags | O_CLOEXEC)};
	if (!fd)
		throw errno_error(std::string{"open "} + path);
	return fd;
}

/// ioctl that restarts on EINTR; returns 0 or -1 with errno set.
template <typename Arg>
int xioctl(int fd, unsigned long request, Arg *arg) noexcept
{
	int r;
	do
		r = ::ioctl(fd, request, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

template <typename Arg>
void ioctl_or_throw(int fd, unsigned long request, Arg *arg, std::string_view what)
{
	if (xioctl(fd, request, arg) < 0)
		throw errno_error(what);
}

} // namespace ar0234

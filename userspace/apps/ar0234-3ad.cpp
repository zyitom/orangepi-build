// SPDX-License-Identifier: GPL-2.0-or-later
//
// ar0234-3ad: run libisp 3A (AE/AWB) for every stream on the AR0234 pipeline.
//
// Any application may stream from /dev/video0; the daemon never opens it (vin
// refuses a second open of a busy node). It watches the ISP sub-device instead:
//   V4L2_EVENT_FRAME_SYNC  queued for the first two frames of each stream
//   V4L2_EVENT_VIN_ISP_OFF queued when the ISP stops
// Per stream it measures the frame rate from the first two frame events, reads
// the sensor window from the ISP sink pad, installs the matching parameter set
// (3DNR off at 1920x1200@120) and forks a child that runs one libisp session
// with the AE exposure capped to the frame period. A fresh process per stream
// keeps libisp's global state clean.

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include <getopt.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ar0234/isp3a.hpp"
#include "ar0234/v4l2.hpp"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Options {
	std::string isp_name = "sunxi_isp.0";
	int video_id = 0;
	ar0234::IspParamSets params;
	bool verbose = false;
};

void print_log(const char *fmt, auto... args)
{
	std::fprintf(stderr, fmt, args...);
	std::fputc('\n', stderr);
}

void usage()
{
	std::fprintf(stderr,
		     "usage: ar0234-3ad [-i isp_subdev_name] [-v video_id] [-p param_dir] [-V]\n"
		     "  -i  ISP sub-device name (default sunxi_isp.0)\n"
		     "  -v  vin video node id libisp attaches to (default 0)\n"
		     "  -p  directory with isp_param_3dnr.bin / isp_param_no3dnr.bin\n"
		     "      (default /mnt/extsd/ar0234)\n"
		     "  -V  log every ISP event\n");
}

/// Child process: one libisp session until SIGTERM.
[[noreturn]] void run_session(const Options &opt, ar0234::Resolution sensor, int fps)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigprocmask(SIG_BLOCK, &set, nullptr);
	try {
		if (auto installed = opt.params.select(sensor, fps))
			print_log("ar0234-3ad: parameters %s", installed->c_str());
		ar0234::Isp3A session{opt.video_id, fps};
		print_log("ar0234-3ad: 3A running on isp%d for %ux%u@%d", session.isp_id(), sensor.width,
		    sensor.height, fps);
		int sig;
		sigwait(&set, &sig);
	} catch (const std::exception &e) {
		print_log("ar0234-3ad: %s", e.what());
		std::_Exit(1);
	}
	std::_Exit(0);
}

class SessionProcess {
public:
	SessionProcess() = default;
	SessionProcess(const SessionProcess &) = delete;
	SessionProcess &operator=(const SessionProcess &) = delete;
	~SessionProcess() { stop(); }

	[[nodiscard]] bool running() const noexcept { return pid_ > 0; }

	void start(const Options &opt, ar0234::Resolution sensor, int fps)
	{
		stop();
		std::fflush(nullptr);
		pid_ = ::fork();
		if (pid_ == 0)
			run_session(opt, sensor, fps);
		if (pid_ < 0)
			print_log("ar0234-3ad: fork failed");
	}

	void stop() noexcept
	{
		if (pid_ <= 0)
			return;
		::kill(pid_, SIGTERM);
		const auto deadline = Clock::now() + 3s;
		while (::waitpid(pid_, nullptr, WNOHANG) == 0) {
			if (Clock::now() > deadline) {
				print_log("ar0234-3ad: 3A session did not stop, killing it");
				::kill(pid_, SIGKILL);
				::waitpid(pid_, nullptr, 0);
				break;
			}
			::usleep(10'000);
		}
		pid_ = -1;
	}

	/// Collect a session that exited on its own (libisp failure).
	void reap() noexcept
	{
		int status = 0;
		if (pid_ > 0 && ::waitpid(pid_, &status, WNOHANG) == pid_) {
			print_log("ar0234-3ad: 3A session exited (status %d)", WEXITSTATUS(status));
			pid_ = -1;
		}
	}

private:
	pid_t pid_ = -1;
};

/// /run/ar0234-3ad.pid lets capture tools tell whether 3A is available.
class PidFile {
public:
	explicit PidFile(std::filesystem::path path) : path_{std::move(path)}
	{
		std::ofstream out{path_, std::ios::trunc};
		if (!(out << ::getpid() << '\n'))
			path_.clear();
	}
	PidFile(const PidFile &) = delete;
	PidFile &operator=(const PidFile &) = delete;
	~PidFile()
	{
		if (!path_.empty()) {
			std::error_code ec;
			std::filesystem::remove(path_, ec);
		}
	}

private:
	std::filesystem::path path_;
};

std::optional<Options> parse(int argc, char **argv)
{
	Options opt;
	for (int c; (c = ::getopt(argc, argv, "i:v:p:Vh")) != -1;) {
		switch (c) {
		case 'i': opt.isp_name = optarg; break;
		case 'v': opt.video_id = std::atoi(optarg); break;
		case 'p': opt.params.dir = optarg; break;
		case 'V': opt.verbose = true; break;
		default: return std::nullopt;
		}
	}
	return opt;
}

} // namespace

int main(int argc, char **argv)
{
	const auto opt = parse(argc, argv);
	if (!opt) {
		usage();
		return 2;
	}

	struct sigaction sa{};
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);

	try {
		const auto node = ar0234::find_v4l2_node(opt->isp_name);
		if (!node) {
			print_log("ar0234-3ad: no V4L2 node named %s (is vin_v4l2 loaded?)", opt->isp_name.c_str());
			return 1;
		}
		ar0234::Subdev isp{*node};
		isp.subscribe({V4L2_EVENT_FRAME_SYNC, ar0234::kEventVinIspOff});
		print_log("ar0234-3ad: watching %s (%s)", opt->isp_name.c_str(), node->c_str());

		const PidFile pid_file{"/run/ar0234-3ad.pid"};
		SessionProcess session;
		// timestamp of the first frame event of a stream still waiting for its second
		std::optional<std::chrono::nanoseconds> first_frame;
		std::optional<ar0234::Resolution> sensor;
		Clock::time_point first_seen;

		const auto start_session = [&](int fps) {
			print_log("ar0234-3ad: stream %ux%u, %d fps", sensor->width, sensor->height, fps);
			session.start(*opt, *sensor, fps);
			first_frame.reset();
		};

		while (!g_stop) {
			session.reap();
			const auto ev = isp.next_event(100ms);

			// no second frame event (very low frame rate, external trigger):
			// run without an AE frame-period cap
			if (!ev) {
				if (first_frame && Clock::now() - first_seen > 500ms)
					start_session(0);
				continue;
			}
			if (opt->verbose)
				print_log("ar0234-3ad: event 0x%x seq %u", ev->type, ev->sequence);

			if (ev->type == ar0234::kEventVinIspOff) {
				first_frame.reset();
				if (session.running()) {
					session.stop();
					print_log("ar0234-3ad: stream stopped");
				}
				continue;
			}
			if (ev->type != V4L2_EVENT_FRAME_SYNC || session.running())
				continue;

			const auto t = ar0234::event_time(*ev);
			if (!first_frame) {
				first_frame = t;
				first_seen = Clock::now();
				sensor = isp.pad_format(0).value_or(ar0234::Resolution{});
				continue;
			}
			const std::chrono::duration<double> period = t - *first_frame;
			start_session(period.count() > 0 ? static_cast<int>(std::lround(1.0 / period.count())) : 0);
		}
	} catch (const std::exception &e) {
		print_log("ar0234-3ad: %s", e.what());
		return 1;
	}
	return 0;
}

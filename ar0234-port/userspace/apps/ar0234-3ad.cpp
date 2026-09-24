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

#include "ar0234/ar0234conf.hpp"
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
	std::filesystem::path conf_path = "/etc/ar0234.conf";
	ar0234::Ar0234Conf conf;
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
		     "usage: ar0234-3ad [-i isp_subdev_name] [-v video_id] [-p param_dir]"
		     " [-c conf_file] [-V]\n"
		     "  -i  ISP sub-device name (default sunxi_isp.0)\n"
		     "  -v  vin video node id libisp attaches to (default 0)\n"
		     "  -p  directory with isp_param_3dnr.bin / isp_param_no3dnr.bin\n"
		     "      (default /mnt/extsd/ar0234)\n"
		     "  -c  fixed-mode config (default /etc/ar0234.conf; missing file =\n"
		     "      auto mode, previous behaviour)\n"
		     "  -V  log every ISP event\n");
}

/// Child process.
///
/// AUTO mode (default): one libisp session until SIGTERM, as before.
///
/// FIXED mode (/etc/ar0234.conf mode=fixed): NO libisp at all. Measured
/// round 11: libisp's manual mode (param-file byte patch) still rewrites the
/// sensor every frame -- exposure honours the pinned value but the gain field
/// always arrives as the 1.0x clamp minimum, so a "fixed gain" could never
/// stick and a direct sensor-subdev write loses the race against 120 Hz
/// rewrites. Without a libisp session there is no writer at all: the pinned
/// exposure and gain are written once through the sensor sub-device (which is
/// never busy) and hold for the whole stream (verified: gain 16x, Y 7 -> 82).
/// The ISP itself runs on the configuration the kernel restores from the
/// ctx saved by the last auto session (/mnt/isp0_*.ctx_saved.bin) or on its
/// power-on default, so run one normal auto stream after installing before
/// relying on fixed mode. AWB stays at the last auto session's gains in this
/// mode; pinning white balance needs a fully regenerated parameter file
/// (tools/make_isp_bin.py), not a byte patch.
[[noreturn]] void run_session(const Options &opt, ar0234::Resolution sensor, int fps)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigprocmask(SIG_BLOCK, &set, nullptr);

	if (opt.conf.fixed()) {
		try {
			auto node = ar0234::find_v4l2_node("ar0234_mipi");
			if (!node)
				throw std::runtime_error{"no ar0234_mipi subdev node"};
			ar0234::Subdev sensor_dev{*node};
			if (opt.conf.exp_val_16()) {
				sensor_dev.set_control(0x00980911, // exposure, 1/16 line
						       static_cast<std::int32_t>(
							       *opt.conf.exp_val_16()));
				print_log("ar0234-3ad: fixed exposure %u lines",
					  *opt.conf.exp_val_16() / 16);
			}
			if (opt.conf.gain_16()) {
				sensor_dev.set_control(0x00980913, // gain, 1/1600 x
						       static_cast<std::int32_t>(
							       *opt.conf.gain_16()) *
							       100);
				print_log("ar0234-3ad: fixed gain %.2fx",
					  *opt.conf.gain_16() / 16.0);
			}
			print_log("ar0234-3ad: fixed passthrough via %s (no libisp, no AE)",
				  node->c_str());
		} catch (const std::exception &e) {
			print_log("ar0234-3ad: fixed mode failed: %s", e.what());
			std::_Exit(1);
		}
		int sig;
		sigwait(&set, &sig);
		std::_Exit(0);
	}

	try {
		if (auto installed = opt.params.select(sensor, fps, opt.conf.params()))
			print_log("ar0234-3ad: parameters %s", installed->c_str());
		else if (opt.conf.ae_log()) {
			/* diagnostics switch: works in auto mode too, so the AE can
			 * be watched while it adapts (fps-boundary hunts etc.) */
			const ar0234::FixedParams log_only{.ae_log = true};
			if (auto patched = ar0234::apply_fixed_mode(log_only, opt.params.active))
				print_log("ar0234-3ad: ae_log enabled in %s (auto mode)",
					  patched->c_str());
		}
		ar0234::Isp3A session{opt.video_id, fps > 120 ? 120 : fps};
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
	for (int c; (c = ::getopt(argc, argv, "i:v:p:c:Vh")) != -1;) {
		switch (c) {
		case 'i': opt.isp_name = optarg; break;
		case 'v': opt.video_id = std::atoi(optarg); break;
		case 'p': opt.params.dir = optarg; break;
		case 'c': opt.conf_path = optarg; break;
		case 'V': opt.verbose = true; break;
		default: return std::nullopt;
		}
	}
	return opt;
}

} // namespace

int main(int argc, char **argv)
{
	auto opt = parse(argc, argv);
	if (!opt) {
		usage();
		return 2;
	}

	struct sigaction sa{};
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);

	try {
		opt->conf = ar0234::Ar0234Conf::load(opt->conf_path);
		if (opt->conf.present()) {
			print_log("ar0234-3ad: config %s (%s)", opt->conf_path.c_str(),
				  opt->conf.fixed() ? "fixed mode" : "auto mode");
			for (const auto &w : opt->conf.warnings())
				print_log("ar0234-3ad: config warning: %s", w.c_str());
		}

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

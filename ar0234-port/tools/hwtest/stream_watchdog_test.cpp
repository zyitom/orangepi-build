// SPDX-License-Identifier: GPL-2.0-or-later
//
// stream_watchdog_test: measure the "stream started but produced almost no
// frames" failure of the AR0234 -> ISP pipeline and how much of it the
// Capture stream-start watchdog absorbs.
//
//   tools/stream_watchdog_test -n 40 -t 4 -w 1920 -h 1200 -f 120
//   tools/stream_watchdog_test -n 40 -t 4 -w 1920 -h 1200 -f 120 --off
//
// One iteration = open the node, STREAMON, count frames for -t seconds, close.
// A run is "bad" when it collects less than half of the frames a healthy
// stream would deliver, which is exactly the shape of the known failure
// (frames=4, wall=4 s, fps~1, vi0 frame counter back to 0, silent dmesg).
//
// With --off the watchdog is disabled (probe_timeout_ms = 0) so the raw
// failure rate shows through; with the watchdog on every bad start should be
// recovered by a restart and no iteration should end bad.

#include <ar0234/v4l2.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <getopt.h>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
	ar0234::CaptureConfig capture;
	unsigned iterations = 40;
	double seconds = 4.0;
	bool watchdog = true;
};

void usage()
{
	std::fprintf(stderr,
		     "usage: stream_watchdog_test [-n iterations] [-t seconds] [-d dev]\n"
		     "                             [-w width] [-h height] [-f fps] [--off]\n"
		     "  -n   number of stream starts (default 40)\n"
		     "  -t   seconds of streaming per start (default 4)\n"
		     "  --off  disable the stream-start watchdog (raw failure rate)\n");
}

std::optional<Options> parse(int argc, char **argv)
{
	static const option long_opts[] = {
		{"off", no_argument, nullptr, 1000},
		{nullptr, 0, nullptr, 0},
	};
	Options opt;
	for (int c; (c = ::getopt_long(argc, argv, "n:t:d:w:h:f:", long_opts, nullptr)) != -1;) {
		switch (c) {
		case 'n': opt.iterations = std::strtoul(optarg, nullptr, 0); break;
		case 't': opt.seconds = std::atof(optarg); break;
		case 'd': opt.capture.device = optarg; break;
		case 'w': opt.capture.size.width = std::strtoul(optarg, nullptr, 0); break;
		case 'h': opt.capture.size.height = std::strtoul(optarg, nullptr, 0); break;
		case 'f': opt.capture.fps = std::atoi(optarg); break;
		case 1000: opt.watchdog = false; break;
		default: return std::nullopt;
		}
	}
	if (!opt.watchdog)
		opt.capture.probe_timeout_ms = 0;
	return opt;
}

struct Result {
	bool bad = false;
	long frames = 0;
	double wall = 0;
	unsigned retries = 0;
	unsigned failed_probes = 0;
	bool threw = false;
	std::string error;
};

Result run_once(const Options &opt)
{
	Result res;
	const auto t0 = Clock::now();
	std::optional<ar0234::Capture> cap;
	try {
		cap.emplace(opt.capture);
		cap->start();
		const auto t_start = Clock::now();
		long frames = 0;
		while (true) {
			const auto elapsed = std::chrono::duration<double>(Clock::now() - t_start).count();
			if (elapsed >= opt.seconds)
				break;
			auto frame = cap->dequeue(200ms);
			if (frame)
				++frames;
		}
		res.wall = std::chrono::duration<double>(Clock::now() - t_start).count();
		res.frames = frames;
		cap->stop();
	} catch (const std::exception &e) {
		res.threw = true;
		res.error = e.what();
		if (res.wall == 0)
			res.wall = std::chrono::duration<double>(Clock::now() - t0).count();
	}
	if (cap) {
		const auto &st = cap->stream_stats();
		res.retries = st.retries_used;
		res.failed_probes = st.failed_probes + st.streamon_failures;
	}
	if (res.threw)
		return res;
	// a healthy stream never loses more than half of its frames in a run
	const auto expected = opt.seconds * opt.capture.fps;
	res.bad = res.frames < expected * 0.5;
	return res;
}

} // namespace

int main(int argc, char **argv)
{
	const auto opt = parse(argc, argv);
	if (!opt) {
		usage();
		return 2;
	}

	std::printf("stream_watchdog_test: %s %ux%u@%d, %u starts x %.1f s, watchdog %s\n",
		    opt->capture.device, opt->capture.size.width, opt->capture.size.height,
		    opt->capture.fps, opt->iterations, opt->seconds, opt->watchdog ? "ON" : "OFF");

	unsigned bad = 0, threw = 0, retried = 0, recovered = 0, probes = 0;
	long total_frames = 0;
	double total_wall = 0;

	for (unsigned i = 1; i <= opt->iterations; ++i) {
		const Result r = run_once(*opt);
		total_frames += r.frames;
		total_wall += r.wall;
		if (r.threw)
			++threw;
		if (r.bad)
			++bad;
		if (r.failed_probes > 0) {
			++retried;
			probes += r.failed_probes;
			if (!r.bad && !r.threw)
				++recovered;
		}
		std::printf("iter %2u/%u: frames=%ld wall=%.3fs fps=%.2f retries=%u failed_probes=%u%s%s%s\n",
			    i, opt->iterations, r.frames, r.wall,
			    r.wall > 0 ? r.frames / r.wall : 0.0, r.retries, r.failed_probes,
			    r.bad ? "  BAD" : "", r.threw ? "  THREW" : "", r.threw ? " " : "");
		if (r.threw)
			std::printf("         error: %s\n", r.error.c_str());
		std::fflush(stdout);
	}

	std::printf("\n==== summary ====\n");
	std::printf("starts                 : %u\n", opt->iterations);
	std::printf("frames total           : %ld in %.3f s (%.3f fps overall)\n", total_frames, total_wall,
		    total_wall > 0 ? total_frames / total_wall : 0.0);
	std::printf("failed probes          : %u\n", probes);
	std::printf("starts that retried    : %u  (%.1f%%)\n", retried, 100.0 * retried / opt->iterations);
	std::printf("retries that recovered : %u/%u\n", recovered, retried);
	std::printf("BAD starts remaining   : %u\n", bad);
	std::printf("threw                  : %u\n", threw);
	return bad || threw ? 1 : 0;
}

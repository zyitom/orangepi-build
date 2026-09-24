// SPDX-License-Identifier: GPL-2.0-or-later
//
// exp_linearity: measure how linearly the AR0234 + ISP602 chain maps exposure
// to output brightness. Used to check that the "industrial" ISP parameter set
// (linear gamma, no DRC/PLTM/GTM/defog/sharp/...) really removed the tone
// curve: with the shipping parameter set (gamma + DRC + PLTM all on) the
// response is deliberately non-linear.
//
//   tools/exp_linearity -w 1920 -h 1200 -f 30 -d /dev/video0
//
// The exposure ladder is relative to what auto exposure settled on, so the
// brightest step lands near mid-grey and nothing saturates:
//   e/8, e/4, e/2, e   at the gain auto exposure chose.
// Output: mean luma of the centre half of the frame per step, plus the
// Pearson correlation of mean luma against exposure.

#include <ar0234/v4l2.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <getopt.h>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Options {
	ar0234::CaptureConfig capture;
	std::vector<double> multiples{0.125, 0.25, 0.5, 1.0};
	int gain_q8 = 0; ///< 0 = keep the gain auto exposure settled on
	bool fixed_ladder = false;
	std::vector<int> exposure_us; ///< used with -e
	int settle_ms = 900;
	int frames = 8;
	bool verbose = false;
};

void usage()
{
	std::fprintf(stderr,
		     "usage: exp_linearity [-d dev] [-w W] [-h H] [-f fps] [-g gain]\n"
		     "                     [-m 1/8,1/4,1/2,1] [-e us,us,...] [-s settle_ms] [-n frames]\n"
		     "  default: auto exposure settles first, then the ladder is a fraction of it\n"
		     "  -e     explicit exposure ladder in microseconds (fixed gain)\n"
		     "  -g     manual gain, 1.0 = 1x (default: keep the settled AE gain)\n");
}

std::optional<Options> parse(int argc, char **argv)
{
	Options opt;
	for (int c; (c = ::getopt(argc, argv, "d:w:h:f:g:m:e:s:n:v")) != -1;) {
		switch (c) {
		case 'd': opt.capture.device = optarg; break;
		case 'w': opt.capture.size.width = std::strtoul(optarg, nullptr, 0); break;
		case 'h': opt.capture.size.height = std::strtoul(optarg, nullptr, 0); break;
		case 'f': opt.capture.fps = std::atoi(optarg); break;
		case 'g': opt.gain_q8 = static_cast<int>(std::atof(optarg) * 256 + 0.5); break;
		case 's': opt.settle_ms = std::atoi(optarg); break;
		case 'n': opt.frames = std::atoi(optarg); break;
		case 'v': opt.verbose = true; break;
		case 'm': {
			opt.multiples.clear();
			for (std::string s{optarg}; !s.empty();) {
				const auto comma = s.find(',');
				opt.multiples.push_back(std::atof(s.substr(0, comma).c_str()));
				if (comma == std::string::npos)
					break;
				s.erase(0, comma + 1);
			}
			break;
		}
		case 'e': {
			opt.fixed_ladder = true;
			for (std::string s{optarg}; !s.empty();) {
				const auto comma = s.find(',');
				opt.exposure_us.push_back(std::atoi(s.substr(0, comma).c_str()));
				if (comma == std::string::npos)
					break;
				s.erase(0, comma + 1);
			}
			break;
		}
		default: return std::nullopt;
		}
	}
	if (opt.multiples.empty() && !opt.fixed_ladder)
		return std::nullopt;
	return opt;
}

/// Mean luma of the centre half of the frame (Y plane, NV12).
double mean_luma(const std::span<std::uint8_t> &data, unsigned w, unsigned h)
{
	const unsigned x0 = w / 4, x1 = w - w / 4;
	const unsigned y0 = h / 4, y1 = h - h / 4;
	double sum = 0;
	std::size_t n = 0;
	for (unsigned y = y0; y < y1; ++y) {
		const std::uint8_t *row = data.data() + std::size_t{y} * w;
		for (unsigned x = x0; x < x1; ++x) {
			sum += row[x];
			++n;
		}
	}
	return n ? sum / n : 0;
}

int get_control(ar0234::Capture &cap, std::uint32_t id)
{
	try {
		return cap.get_control(id);
	} catch (const std::exception &) {
		return -1;
	}
}

/// Average `frames` frames after throwing away `skip` of them: the frames
/// right after a control change still carry the previous exposure, which
/// otherwise poisons the first ladder step.
double measure(ar0234::Capture &cap, int frames, int skip = 6)
{
	double sum = 0;
	int got = 0;
	for (int i = 0; i < (frames + skip) * 4 && got < frames; ++i) {
		auto frame = cap.dequeue(500ms);
		if (!frame)
			continue;
		if (skip > 0) {
			--skip;
			continue;
		}
		sum += mean_luma(cap.buffers()[frame->index()].data(), cap.size().width, cap.size().height);
		++got;
	}
	return got ? sum / got : -1;
}

} // namespace

int main(int argc, char **argv)
{
	const auto opt = parse(argc, argv);
	if (!opt) {
		usage();
		return 2;
	}

	try {
		ar0234::Capture cap{opt->capture};
		cap.start();
		const auto &st = cap.stream_stats();
		std::printf("stream: %u failed probe(s), %u reopen(s)\n", st.failed_probes, st.hard_reopens);

		// libisp only notices control changes while it is running: ar0234-3ad
		// starts it on the first frames of the stream, so wait before touching
		// anything.
		std::this_thread::sleep_for(1500ms);
		cap.set_control(V4L2_CID_AUTOGAIN, 1);
		cap.set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO);
		std::this_thread::sleep_for(2500ms);

		int ae_exp = get_control(cap, V4L2_CID_EXPOSURE_ABSOLUTE);
		int ae_gain = get_control(cap, V4L2_CID_GAIN);
		std::printf("auto exposure settled at %d us, gain code %d (%.2fx)\n", ae_exp, ae_gain,
			    ae_gain / 256.0);
		if (ae_exp <= 0 || ae_gain <= 0) {
			std::fprintf(stderr, "FAIL: could not read the settled exposure/gain\n");
			return 1;
		}

		std::vector<int> ladder;
		if (opt->fixed_ladder)
			ladder = opt->exposure_us;
		else
			for (double m : opt->multiples)
				ladder.push_back(static_cast<int>(ae_exp * m + 0.5));

		// manual exposure and gain from here on; by default keep the gain the
		// auto exposure chose, so only the exposure window moves
		const int gain = opt->gain_q8 > 0 ? opt->gain_q8 : ae_gain;
		cap.set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
		cap.set_control(V4L2_CID_AUTOGAIN, 0);
		cap.set_control(V4L2_CID_GAIN, gain);

		std::printf("ladder: gain %.2fx (code %d), %zu steps\n", gain / 256.0, gain, ladder.size());
		std::vector<double> xs, ys;
		for (int e : ladder) {
			cap.set_control(V4L2_CID_EXPOSURE_ABSOLUTE, e);
			std::this_thread::sleep_for(std::chrono::milliseconds{opt->settle_ms});
			const double luma = measure(cap, opt->frames);
			std::printf("  exposure %6d us -> mean luma %7.3f%s\n", e, luma,
				    luma > 250 ? "  (near/at saturation)" : "");
			xs.push_back(e);
			ys.push_back(luma);
		}
		cap.stop();

		// Pearson r over the unsaturated steps
		std::vector<double> ux, uy;
		for (std::size_t i = 0; i < xs.size(); ++i)
			if (ys[i] <= 250) {
				ux.push_back(xs[i]);
				uy.push_back(ys[i]);
			}
		if (ux.size() < 3) {
			std::printf("RESULT: only %zu unsaturated step(s), cannot fit\n", ux.size());
			return 1;
		}
		double mx = 0, my = 0;
		for (std::size_t i = 0; i < ux.size(); ++i) {
			mx += ux[i] / ux.size();
			my += uy[i] / uy.size();
		}
		double sxy = 0, sxx = 0, syy = 0;
		for (std::size_t i = 0; i < ux.size(); ++i) {
			sxy += (ux[i] - mx) * (uy[i] - my);
			sxx += (ux[i] - mx) * (ux[i] - mx);
			syy += (uy[i] - my) * (uy[i] - my);
		}
		const double r = (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0;
		const double slope = sxx > 0 ? sxy / sxx : 0;
		std::printf("RESULT: unsaturated steps %zu, Pearson r = %.5f, slope %.5f luma/us\n",
			    ux.size(), r, slope);
		return r > 0.99 ? 0 : 1;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}

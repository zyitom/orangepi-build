// SPDX-License-Identifier: GPL-2.0-or-later
//
// ar0234-rec: record H.264/H.265 from the AR0234 with A733 hardware only
//   sunxi-vin ISP + scaler (NV12) -> cedar VE encoder, zero-copy DMA-BUF import
// 3A (AE/AWB) comes from the ar0234-3ad service, which attaches to every stream.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <getopt.h>

#include "ar0234/encoder.hpp"
#include "ar0234/v4l2.hpp"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Options {
	ar0234::CaptureConfig capture;
	ar0234::EncoderConfig encoder;
	long frames = 300; // 0: until SIGINT/SIGTERM
	std::string output;
	bool copy = false;
	std::optional<std::int32_t> exposure_us; // manual exposure through libisp
	std::optional<std::int32_t> gain_q8;     // manual gain, 256 = 1x
};

/// Manual exposure/gain through libisp, which reads them from the video node
/// controls but only notices changes made after it attached to the stream
/// (ar0234-3ad starts it on the first frames). The ISP control handler keeps
/// its values across streams, so auto modes are restored on destruction.
class ManualExposure {
public:
	ManualExposure(ar0234::Capture &capture, const Options &opt) : capture_{capture}, opt_{opt} {}
	ManualExposure(const ManualExposure &) = delete;
	ManualExposure &operator=(const ManualExposure &) = delete;
	~ManualExposure()
	{
		try {
			if (opt_.exposure_us)
				capture_.set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO);
			if (opt_.gain_q8)
				capture_.set_control(V4L2_CID_AUTOGAIN, 1);
		} catch (const std::exception &) {
		}
	}

	void apply()
	{
		if (opt_.exposure_us) {
			capture_.set_control(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
			capture_.set_control(V4L2_CID_EXPOSURE_ABSOLUTE, *opt_.exposure_us);
		}
		if (opt_.gain_q8) {
			capture_.set_control(V4L2_CID_AUTOGAIN, 0);
			capture_.set_control(V4L2_CID_GAIN, *opt_.gain_q8);
		}
		if (opt_.exposure_us || opt_.gain_q8)
			std::printf("manual exposure %d us, gain %.2fx\n", opt_.exposure_us.value_or(0),
				    opt_.gain_q8.value_or(0) / 256.0);
	}

private:
	ar0234::Capture &capture_;
	const Options &opt_;
};

void usage()
{
	std::fprintf(stderr,
		     "usage: ar0234-rec [options]\n"
		     "  -w WIDTH      (1920)      -h HEIGHT (1080)      -f FPS (30)\n"
		     "  -n FRAMES     (300, 0 = until Ctrl-C)\n"
		     "  -b BITRATE    bits/s (8000000)\n"
		     "  -c CODEC      h264 | h265 (h264)\n"
		     "  -o FILE       output (ar0234.h264 / ar0234.h265)\n"
		     "  -e MICROSEC   manual exposure time (default: auto exposure)\n"
		     "  -g GAIN       manual gain, e.g. 2.5 (default: auto gain)\n"
		     "  -C            copy frames into VE buffers instead of zero-copy\n");
}

std::optional<Options> parse(int argc, char **argv)
{
	Options opt;
	for (int c; (c = ::getopt(argc, argv, "w:h:f:n:b:c:o:e:g:C")) != -1;) {
		switch (c) {
		case 'w': opt.capture.size.width = std::strtoul(optarg, nullptr, 0); break;
		case 'h': opt.capture.size.height = std::strtoul(optarg, nullptr, 0); break;
		case 'f': opt.capture.fps = std::atoi(optarg); break;
		case 'n': opt.frames = std::atol(optarg); break;
		case 'b': opt.encoder.bitrate = std::atoi(optarg); break;
		case 'c':
			if (std::string_view{optarg} == "h264")
				opt.encoder.codec = ar0234::Codec::H264;
			else if (std::string_view{optarg} == "h265")
				opt.encoder.codec = ar0234::Codec::H265;
			else
				return std::nullopt;
			break;
		case 'o': opt.output = optarg; break;
		case 'e': opt.exposure_us = std::atoi(optarg); break;
		case 'g': opt.gain_q8 = static_cast<std::int32_t>(std::atof(optarg) * 256 + 0.5); break;
		case 'C': opt.copy = true; break;
		default: return std::nullopt;
		}
	}
	if (opt.capture.fps <= 0 || opt.frames < 0)
		return std::nullopt;
	if (opt.output.empty())
		opt.output = opt.encoder.codec == ar0234::Codec::H264 ? "ar0234.h264" : "ar0234.h265";
	opt.encoder.fps = opt.capture.fps;
	return opt;
}

bool service_running()
{
	std::ifstream in{"/run/ar0234-3ad.pid"};
	long pid = 0;
	// EPERM: the root-owned service exists but we may not signal it
	return (in >> pid) && pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
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
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);

	try {
		ar0234::Capture capture{opt->capture};
		const auto size = capture.size();
		opt->encoder.size = size;
		std::printf("capture %ux%u NV12 @%d fps, %s %.1f Mbit/s\n", size.width, size.height,
			    opt->capture.fps, ar0234::to_string(opt->encoder.codec).data(),
			    opt->encoder.bitrate / 1e6);
		if (!service_running())
			std::printf("warning: ar0234-3ad is not running, no auto exposure / white balance\n");

		ar0234::VideoEncoder encoder{opt->encoder};
		if (!opt->copy && !(capture.dmabuf_exported() && encoder.import_buffers(capture.buffers())))
			std::printf("VE IOMMU import failed, copying frames\n");
		std::printf("input path: %s\n", encoder.zero_copy() ? "zero-copy DMA-BUF (ISP buffer -> VE)" : "memcpy");

		std::ofstream out{opt->output, std::ios::binary | std::ios::trunc};
		if (!out)
			throw std::runtime_error{"cannot open " + opt->output};
		std::uint64_t bytes = 0;
		const ar0234::BitstreamSink sink = [&](std::span<const std::uint8_t> chunk) {
			out.write(reinterpret_cast<const char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
			bytes += chunk.size();
		};
		sink(encoder.stream_header());

		capture.start();
		long got = 0;
		Seconds encode_time{};
		const auto t0 = Clock::now();
		auto next_report = t0 + 2s;
		ManualExposure manual{capture, *opt};
		bool manual_applied = false;

		while (!g_stop && (opt->frames == 0 || got < opt->frames)) {
			if (!manual_applied && Clock::now() - t0 >= 1s) {
				manual.apply();
				manual_applied = true;
			}
			auto frame = capture.dequeue(2s);
			if (!frame) {
				if (!g_stop)
					std::fprintf(stderr, "capture timeout\n");
				break;
			}
			const auto te = Clock::now();
			encoder.encode(frame->index(), capture.buffers()[frame->index()].data(), frame->pts_us(), sink);
			encode_time += Clock::now() - te;
			++got;

			if (const auto now = Clock::now(); now >= next_report) {
				const Seconds el = now - t0;
				std::printf("%ld frames, %.2f fps, encode %.1f ms/frame, %.2f Mbit/s\n", got,
					    got / el.count(), encode_time.count() * 1e3 / got,
					    bytes * 8.0 / el.count() / 1e6);
				std::fflush(stdout);
				next_report = now + 2s;
			}
		}
		const Seconds el = Clock::now() - t0;
		capture.stop();
		std::printf("done: %ld frames in %.2fs (%.2f fps), %llu bytes, encode %.1f ms/frame -> %s\n", got,
			    el.count(), got / el.count(), static_cast<unsigned long long>(bytes),
			    got ? encode_time.count() * 1e3 / got : 0.0, opt->output.c_str());
		return (opt->frames == 0 || got == opt->frames) ? 0 : 3;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "ar0234-rec: %s\n", e.what());
		return 1;
	}
}

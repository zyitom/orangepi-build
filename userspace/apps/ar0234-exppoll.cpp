// SPDX-License-Identifier: GPL-2.0-or-later
//
// ar0234-exppoll -- stream while polling the sensor exposure/gain controls from
// inside the capturing process, to answer the question v4l2-ctl cannot (vin
// refuses a second open of a busy node): what does the sensor ACTUALLY run,
// frame by frame, and what happens to the reported values when the stream
// stops? This is the evidence tool for the NEXT-TASKS N3 finding ("after the
// stream the gain control reads back 1.0x while the AE log said 4x").
//
// Each poll also prints the CPU-read frame content statistics (luma mean/max
// straight from the mmap'ed capture buffer -- no G2D involved), which is the
// bisection tool for "black frames": if the buffer itself is black the
// sensor/ISP is the place to look, not the consumers.
//
// --test-pattern N (0..4, sensor register 0x3070 via the sensor sub-device)
// makes the SENSOR generate the image, scene-independent: with the pattern
// visible the whole ISP->VIN path is proven alive and a black free-running
// image just means the scene is dark (AE saturates at the fps-capped exposure
// and can do nothing about it).
//
// USAGE:  ar0234-exppoll [--seconds N] [--fps N] [--w W] [--h H] [--interval ms]
//                        [--test-pattern 0..4]
// Units: exposure control = 1/16 sensor line (6.8 us/line on every AR0234
// mode), gain control = 1/1600 x. Both controls are volatile - they report
// what the driver last had written (by libisp or by a direct control write).

#include <ar0234/v4l2.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned seconds = 20, fps = 120, w = 640, h = 400, interval_ms = 500;
	int test_pattern = -1;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--seconds" && i + 1 < argc)
			seconds = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--fps" && i + 1 < argc)
			fps = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--w" && i + 1 < argc)
			w = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--h" && i + 1 < argc)
			h = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--interval" && i + 1 < argc)
			interval_ms = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--test-pattern" && i + 1 < argc)
			test_pattern = atoi(argv[++i]);
		else {
			std::printf("usage: %s [--seconds N] [--fps N] [--w W] [--h H]"
				    " [--interval ms] [--test-pattern 0..4]\n",
				    argv[0]);
			return 2;
		}
	}

	constexpr std::uint32_t kExposure = 0x00980911;      // 1/16 line, volatile
	constexpr std::uint32_t kGain = 0x00980913;          // 1/1600 x, volatile
	constexpr std::uint32_t kTestPattern = 0x009f0903;   // sensor subdev menu

	ar0234::CaptureConfig cc{};
	cc.size = {w, h};
	cc.fps = static_cast<int>(fps);
	ar0234::Capture cap{cc};
	cap.start();
	std::printf("streaming %ux%u@%u, polling every %ums\n", w, h, fps, interval_ms);

	std::optional<ar0234::Subdev> sensor;
	if (test_pattern >= 0) {
		auto node = ar0234::find_v4l2_node("ar0234_mipi");
		if (!node) {
			std::printf("no ar0234_mipi subdev node, test pattern unavailable\n");
		} else {
			sensor.emplace(*node);
			try {
				sensor->set_control(kTestPattern, test_pattern);
				std::printf("sensor test pattern %d ON (scene-independent)\n",
					    test_pattern);
			} catch (const std::exception &e) {
				std::printf("set test pattern failed: %s\n", e.what());
			}
		}
	}

	const auto t0 = std::chrono::steady_clock::now();
	unsigned frames = 0;
	while (true) {
		auto fr = cap.dequeue(std::chrono::milliseconds{interval_ms});
		if (!fr) {
			std::printf("  dequeue stall\n");
			continue;
		}
		const auto &buf = cap.buffers()[fr->index()];
		const auto &s = buf.data();
		std::uint64_t sum = 0;
		std::uint8_t max = 0;
		/* luma plane only, stride-sampled: enough for black/white calls */
		for (std::size_t i = 0; i < w * h; i += 7) {
			sum += s[i];
			if (s[i] > max)
				max = s[i];
		}
		const unsigned n = static_cast<unsigned>(w * h / 7 + 1);
		fr.reset();
		++frames;
		const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0);
		if (el.count() >= seconds * 1000)
			break;
		if (frames % std::max(1u, 1000u / interval_ms) == 0) {
			std::int32_t exp = 0, gain = 0;
			try {
				exp = cap.get_control(kExposure);
				gain = cap.get_control(kGain);
			} catch (const std::exception &e) {
				std::printf("  ctrl read failed: %s\n", e.what());
			}
			std::printf("t=%5.1fs exp=%d (%u lines) gain=%d (%.2fx) | Y mean=%.1f"
				    " max=%u\n", el.count() / 1000.0, exp, exp / 16, gain,
				    gain / 1600.0, static_cast<double>(sum) / n, max);
		}
	}

	if (sensor && test_pattern >= 0) {
		try {
			sensor->set_control(kTestPattern, 0);
			std::printf("sensor test pattern OFF\n");
		} catch (const std::exception &e) {
			std::printf("clear test pattern failed: %s\n", e.what());
		}
	}
	cap.stop();
	try {
		std::int32_t exp = cap.get_control(kExposure);
		std::int32_t gain = cap.get_control(kGain);
		std::printf("after stop: exposure=%d (%u lines) gain=%d (%.2fx)\n",
			    exp, exp / 16, gain, gain / 1600.0);
	} catch (const std::exception &e) {
		std::printf("after stop: ctrl read failed: %s\n", e.what());
	}
	return 0;
}

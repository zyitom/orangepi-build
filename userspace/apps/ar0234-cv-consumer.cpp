// SPDX-License-Identifier: GPL-2.0-or-later
//
// ar0234-cv-consumer -- a REAL OpenCV consumer on the AR0234 pipeline
// (NEXT-TASKS N1). This is the "OpenCV side" of the "one frame to NPU +
// OpenCV" plan: the same capture frame that the zero-copy path
// (ar0234-npu-zerocopy --fanout) feeds to the NPU is moved bit-exact by the
// G2D into a second dma-buf, and this program shows a production-shaped
// OpenCV consumer reading exactly that buffer:
//
//   /dev/video0 NV12 (dma-buf exported)
//     -> G2D two Y8 blits (bit-exact move, device side) -> consumer dma-buf
//        -> cv::Mat luma + chroma views OVER the mapped dma-buf (zero copy)
//        -> cv::cvtColorTwoPlane NV12 -> BGR
//        -> whatever OpenCV processing (mean / resize / imwrite here)
//
// The CPU work starts at the cv::Mat views; everything before is device DMA.
// The per-frame OpenCV conversion cost is printed as the honest contrast to
// the NPU-side zero-copy path.
//
// USAGE: ar0234-cv-consumer [--frames N] [--fps N] [--w W] [--h H]
//                          [--dump-every N] [--dump-dir DIR]
// BUILD (on the board): make cv-consumer   (needs libopencv-dev + pkg-config)

#include <ar0234/g2d.hpp>
#include <ar0234/v4l2.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char **argv)
{
	unsigned frames = 120, fps = 120, w = 640, h = 400, dump_every = 0;
	std::filesystem::path dump_dir = "/tmp/cv-consumer";
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc)
			frames = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--fps" && i + 1 < argc)
			fps = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--w" && i + 1 < argc)
			w = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--h" && i + 1 < argc)
			h = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--dump-every" && i + 1 < argc)
			dump_every = static_cast<unsigned>(atoi(argv[++i]));
		else {
			std::printf("usage: %s [--frames N] [--fps N] [--w W] [--h H]"
				    " [--dump-every N] [--dump-dir DIR]\n",
				    argv[0]);
			return 2;
		}
	}

	ar0234::CaptureConfig cc{};
	cc.size = {w, h};
	cc.fps = static_cast<int>(fps);
	ar0234::Capture cap{cc};
	if (!cap.dmabuf_exported())
		std::fprintf(stderr, "warning: capture buffers not dma-buf exported;"
				     " the G2D move will fail\n");
	ar0234::G2d g2d;
	ar0234::DmaBuffer consumer{ar0234::G2d::nv12_size(w, h)};
	std::printf("capture %ux%u@%u; OpenCV %s; consumer dma-buf %zu B\n", w, h, fps,
		    CV_VERSION, consumer.size());

	double sum_move = 0, sum_cv = 0, sum_e2e = 0, max_cv = 0;
	unsigned n = 0;
	cap.start();

	for (unsigned i = 0; i < frames; ++i) {
		auto fr = cap.dequeue(std::chrono::seconds{2});
		if (!fr)
			continue;
		const auto t0 = std::chrono::steady_clock::now();

		/* device-side bit-exact move into the consumer buffer; the CPU
		 * never touches the capture buffer */
		g2d.move_nv12(cap.buffers()[fr->index()].dmabuf.get(), consumer.fd(),
			      w, h);
		const auto t1 = std::chrono::steady_clock::now();

		/* OpenCV views straight over the mapped dma-buf: no copy here */
		consumer.cpu_read_begin();
		cv::Mat y{static_cast<int>(h), static_cast<int>(w), CV_8UC1,
			  consumer.data()};
		cv::Mat uv{static_cast<int>(h / 2), static_cast<int>(w / 2), CV_8UC2,
			   consumer.data() + std::size_t{w} * h};
		cv::Mat bgr;
		cv::cvtColorTwoPlane(y, uv, bgr, cv::COLOR_YUV2BGR_NV12);
		const cv::Scalar mean = cv::mean(bgr);
		const auto t2 = std::chrono::steady_clock::now();
		consumer.cpu_read_end();

		double move_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		double cv_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
		sum_move += move_ms;
		sum_cv += cv_ms;
		sum_e2e += move_ms + cv_ms;
		max_cv = std::max(max_cv, cv_ms);
		++n;

		if (dump_every && n % dump_every == 0) {
			std::filesystem::create_directories(dump_dir);
			char name[64];
			std::snprintf(name, sizeof name, "frame%u.png", n);
			cv::imwrite((dump_dir / name).string(), bgr);
		}
		if (n == frames) { /* keep the last mean in the summary */
			std::printf("last frame BGR mean (B,G,R) = (%.1f, %.1f, %.1f)\n",
				    mean[0], mean[1], mean[2]);
		}
	}

	cap.stop();
	std::printf("\n== %u frames ==\n", n);
	std::printf("G2D bit-exact NV12 move:      %.2f ms avg (device side, no CPU)\n",
		    n ? sum_move / n : 0.0);
	std::printf("OpenCV NV12->BGR + mean:      %.2f ms avg / %.2f ms max (CPU)\n",
		    n ? sum_cv / n : 0.0, max_cv);
	std::printf("capture -> cv::Mat BGR ready: %.2f ms avg\n",
		    n ? sum_e2e / n : 0.0);
	const auto &st = cap.stream_stats();
	std::printf("stream: %u start(s), %u failed probe(s), %u reopen(s)\n",
		    st.start_calls, st.failed_probes, st.hard_reopens);
	return 0;
}

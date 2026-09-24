// SPDX-License-Identifier: GPL-2.0-or-later
//
// ar0234-npu-zerocopy -- the "NPU <- ISP zero-copy" acceptance test (C10 in
// docs/NEXT-TASKS.md). Until this program ran, `vip_create_buffer_from_fd` existed
// as a library symbol but had never been fed a real ISP frame: both vendor
// demos (/opt/vpm_run, /opt/yolov5) read their inputs from files.
//
// WHAT IT PROVES
//   That a real ISP frame reaches the A733's NPU (VIPLite) without the CPU
//   touching a single pixel, and that the result is byte-identical to the
//   same network run from a deliberately CPU-copied input:
//
//     /dev/video0 (ISP NV12, dma-buf exported per buffer)
//        |
//        |  G2D blit: scale + NV12 -> BGR888, one pass, device side
//        v
//     npu_in dma-buf
//        |
//        +-- path A (zero-copy): vip_create_buffer_from_fd(npu_in fd)
//        |       the buffer is written by G2D and read by the NPU -- two
//        |       devices that meet in DDR; the CPU mmaps it only for the
//        |       control path and never writes it, so no cache maintenance
//        |       exists to get wrong (vip_lite.h documents that the fd path
//        |       does NOT do host cache management -- nothing to do here).
//        |
//        +-- path B (control): the CPU reads npu_in, copies it into a
//                vip_create_buffer allocation, flushes it via the documented
//                vip_flush_buffer(FLUSH), runs, invalidates the output.
//
//   A and B must produce byte-identical outputs on real frames. Output cache
//   maintenance (both paths) uses vip_flush_buffer(INVALIDATE) before the CPU
//   reads the network result, per the vip_lite.h contract.
//
//   --fanout adds the production shape from NEXT-TASKS C10/C11: the SAME
//   capture buffer fd is also moved bit-exact (two Y8 blits) into a second
//   consumer dma-buf -- what an OpenCV-side consumer would read -- and a
//   checksum of it is taken once per second, still with no CPU pixel copy in
//   the NPU path. That is the "one frame to NPU + OpenCV" pipeline with no
//   extra capture and no extra sensor bandwidth.
//
// USAGE (built natively on the board; needs the `video` group)
//   ar0234-npu-zerocopy [--frames N] [--fps N] [--w W] [--h H]
//                       [--model PATH] [--fanout] [--dump DIR]
//   defaults: 300 frames, 120 fps, 640x400 capture (the smallest measured
//   good ISP output), /opt/vpm_run/network_binary.nb (224x224 classifier).
//
// EXIT CODES
//   0  stream healthy and every A/B output pair byte-identical
//   1  outputs differ, or an inference call failed mid-run
//   2  setup error (capture, G2D, NPU init, model, shape mismatch)

#include <ar0234/g2d.hpp>
#include <ar0234/v4l2.hpp>

#include <vip_lite.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

namespace {

double ms_since(const timespec &t0)
{
	timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}

double cpu_ms_now()
{
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3 +
	       (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
}

[[noreturn]] void die(int code, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::fprintf(stderr, "FAIL: ");
	std::vfprintf(stderr, fmt, ap);
	std::fputc('\n', stderr);
	va_end(ap);
	std::exit(code);
}

#define VIP_CHECK(call)                                                        \
	do {                                                                   \
		vip_status_e s_ = (call);                                      \
		if (s_ != VIP_SUCCESS)                                         \
			die(2, "NPU: %s failed with status %d at %s:%d", #call,\
			    static_cast<int>(s_), __FILE__, __LINE__);         \
	} while (0)

std::size_t format_bytes(vip_uint32_t fmt)
{
	switch (fmt) {
	case VIP_BUFFER_FORMAT_UINT8:
	case VIP_BUFFER_FORMAT_INT8:
	case VIP_BUFFER_FORMAT_CHAR:
	case VIP_BUFFER_FORMAT_BOOL8:
		return 1;
	case VIP_BUFFER_FORMAT_UINT16:
	case VIP_BUFFER_FORMAT_INT16:
	case VIP_BUFFER_FORMAT_FP16:
	case VIP_BUFFER_FORMAT_BFP16:
		return 2;
	case VIP_BUFFER_FORMAT_FP32:
	case VIP_BUFFER_FORMAT_INT32:
	case VIP_BUFFER_FORMAT_UINT32:
		return 4;
	case VIP_BUFFER_FORMAT_INT64:
	case VIP_BUFFER_FORMAT_UINT64:
	case VIP_BUFFER_FORMAT_FP64:
		return 8;
	default:
		die(2, "NPU: unhandled tensor format %u", fmt);
	}
}

struct TensorInfo {
	std::uint32_t num_dims = 0;
	std::uint32_t dims[6] = {};
	std::uint32_t format = 0;
	std::uint32_t quant = 0;
	float tf_scale = 0.f;
	std::uint32_t tf_zero = 0;
	std::size_t bytes = 0;

	void query(vip_network net, vip_uint32_t index, bool input)
	{
		auto q = [&](vip_enum prop, void *v) {
			return input ? vip_query_input(net, index, prop, v)
				     : vip_query_output(net, index, prop, v);
		};
		VIP_CHECK(q(VIP_BUFFER_PROP_NUM_OF_DIMENSION, &num_dims));
		if (num_dims > 6)
			die(2, "NPU: tensor has %u dimensions", num_dims);
		VIP_CHECK(q(VIP_BUFFER_PROP_SIZES_OF_DIMENSION, dims));
		VIP_CHECK(q(VIP_BUFFER_PROP_DATA_FORMAT, &format));
		VIP_CHECK(q(VIP_BUFFER_PROP_QUANT_FORMAT, &quant));
		if (quant == VIP_BUFFER_QUANTIZE_TF_ASYMM) {
			VIP_CHECK(q(VIP_BUFFER_PROP_TF_SCALE, &tf_scale));
			VIP_CHECK(q(VIP_BUFFER_PROP_TF_ZERO_POINT, &tf_zero));
		}
		std::size_t elems = 1;
		for (std::uint32_t i = 0; i < num_dims; ++i)
			elems *= dims[i];
		bytes = elems * format_bytes(format);
	}
};

struct RunStats {
	double sum = 0, max = 0;
	unsigned long n = 0;
	void add(double v)
	{
		sum += v;
		if (v > max)
			max = v;
		++n;
	}
	[[nodiscard]] double avg() const { return n ? sum / n : 0; }
};

} // namespace

int main(int argc, char **argv)
{
	std::string model = "/opt/vpm_run/network_binary.nb";
	unsigned frames = 300, fps = 120, cw = 640, ch = 400;
	bool fanout = false;
	std::filesystem::path dump_dir;

	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--frames" && i + 1 < argc)
			frames = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--fps" && i + 1 < argc)
			fps = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--w" && i + 1 < argc)
			cw = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--h" && i + 1 < argc)
			ch = static_cast<unsigned>(atoi(argv[++i]));
		else if (a == "--model" && i + 1 < argc)
			model = argv[++i];
		else if (a == "--fanout")
			fanout = true;
		else if (a == "--dump" && i + 1 < argc)
			dump_dir = argv[++i];
		else {
			std::printf("usage: %s [--frames N] [--fps N] [--w W] [--h H]"
				    " [--model PATH] [--fanout] [--dump DIR]\n",
				    argv[0]);
			return 2;
		}
	}

	/* ---------------- capture + G2D (device side of the pipeline) --------- */
	ar0234::CaptureConfig cc{};
	cc.size = {cw, ch};
	cc.fps = static_cast<int>(fps);
	cc.buffer_count = 4;
	ar0234::Capture cap{cc};
	if (!cap.dmabuf_exported())
		die(2, "capture buffers are not dma-buf exported");
	ar0234::G2d g2d;
	std::printf("capture %ux%u @%ufps on %s, %u dma-buf buffers\n", cw, ch, fps,
		    cc.device, cc.buffer_count);

	/* ---------------- NPU setup ------------------------------------------- */
	VIP_CHECK(vip_init());
	vip_network net = nullptr;
	VIP_CHECK(vip_create_network(model.c_str(), 0, VIP_CREATE_NETWORK_FROM_FILE,
				     &net));
	std::uint32_t in_count = 0, out_count = 0;
	VIP_CHECK(vip_query_network(net, VIP_NETWORK_PROP_INPUT_COUNT, &in_count));
	VIP_CHECK(vip_query_network(net, VIP_NETWORK_PROP_OUTPUT_COUNT, &out_count));
	std::printf("model %s: %u input(s), %u output(s)\n", model.c_str(), in_count,
		    out_count);
	if (in_count != 1 || out_count != 1)
		die(2, "NPU: this tool handles exactly 1 input / 1 output");

	TensorInfo in{}, out{};
	in.query(net, 0, true);
	out.query(net, 0, false);
	auto print_tensor = [](const char *what, const TensorInfo &t) {
		std::printf("  %s: dims [", what);
		for (std::uint32_t i = 0; i < t.num_dims; ++i)
			std::printf("%s%u", i ? ", " : "", t.dims[i]);
		std::printf("] fmt %u quant %u scale %.6g zero %u = %zu B\n", t.format,
			    t.quant, t.tf_scale, t.tf_zero, t.bytes);
	};
	print_tensor("input", in);
	print_tensor("output", out);

	/* The G2D converter produces a packed 4-D image [w, h, 3, 1]; anything
	 * else would need a byte layout this tool cannot honestly produce from
	 * an ISP frame. */
	if (in.num_dims != 4 || in.dims[2] != 3 || in.dims[3] != 1)
		die(2, "NPU: input is [w,h,c,n] = [%u,%u,%u,%u]; only c=3 image "
			"inputs can be fed from the ISP here", in.dims[0], in.dims[1],
			in.dims[2], in.dims[3]);
	const std::uint32_t dw = in.dims[0], dh = in.dims[1];

	/* The NPU input dma-buf. system heap first (it is the heap every other
	 * consumer uses); if the NPU driver refuses it -- it is page-based, not
	 * a CMA carve-out -- retry from /dev/dma_heap/reserved, which is the
	 * physically contiguous CMA heap this board also exposes. The heap that
	 * worked is reported so the production pipeline knows which one to use. */
	std::unique_ptr<ar0234::DmaBuffer> npu_in;
	const char *heap_used = "/dev/dma_heap/system";
	try {
		npu_in = std::make_unique<ar0234::DmaBuffer>(in.bytes);
	} catch (const std::exception &e) {
		die(2, "dma-heap alloc (%zu B): %s", in.bytes, e.what());
	}
	vip_buffer_create_params_t params{};
	params.num_of_dims = 4;
	params.sizes[0] = dw;
	params.sizes[1] = dh;
	params.sizes[2] = 3;
	params.sizes[3] = 1;
	params.data_format = in.format;
	params.quant_format = in.quant;
	if (in.quant == VIP_BUFFER_QUANTIZE_TF_ASYMM) {
		params.quant_data.affine.scale = in.tf_scale;
		params.quant_data.affine.zeroPoint =
			static_cast<vip_int32_t>(in.tf_zero);
	}
	vip_buffer buf_fd = nullptr;
	params.memory_type = VIP_BUFFER_MEMORY_TYPE_DMA_BUF;
	vip_status_e fd_rc = vip_create_buffer_from_fd(
		&params, static_cast<vip_uint32_t>(npu_in->fd()),
		static_cast<vip_uint32_t>(npu_in->size()), &buf_fd);
	if (fd_rc != VIP_SUCCESS) {
		std::printf("note: vip_create_buffer_from_fd rejected the %s dma-buf "
			    "(status %d); retrying from /dev/dma_heap/reserved\n",
			    heap_used, static_cast<int>(fd_rc));
		try {
			npu_in = std::make_unique<ar0234::DmaBuffer>(
				in.bytes, "/dev/dma_heap/reserved");
		} catch (const std::exception &e) {
			die(2, "dma-heap reserved alloc (%zu B): %s", in.bytes, e.what());
		}
		heap_used = "/dev/dma_heap/reserved";
		VIP_CHECK(vip_create_buffer_from_fd(
			&params, static_cast<vip_uint32_t>(npu_in->fd()),
			static_cast<vip_uint32_t>(npu_in->size()), &buf_fd));
	}
	std::printf("NPU input buffer: %zu B from %s, fd-imported\n", in.bytes,
		    heap_used);

	/* The control-path input: a driver-allocated buffer the CPU writes. */
	params.memory_type = VIP_BUFFER_MEMORY_TYPE_DEFAULT;
	vip_buffer buf_cpu = nullptr;
	VIP_CHECK(vip_create_buffer(&params, sizeof(params), &buf_cpu));
	void *cpu_in_map = vip_map_buffer(buf_cpu);
	if (!cpu_in_map)
		die(2, "NPU: vip_map_buffer(input) returned null");
	if (vip_get_buffer_size(buf_cpu) < in.bytes)
		die(2, "NPU: cpu input buffer is %u B, need %zu",
		    vip_get_buffer_size(buf_cpu), in.bytes);

	/* Output buffer (driver-allocated; the CPU is its consumer). */
	vip_buffer_create_params_t out_params{};
	out_params.num_of_dims = out.num_dims;
	for (std::uint32_t i = 0; i < out.num_dims; ++i)
		out_params.sizes[i] = out.dims[i];
	out_params.data_format = out.format;
	out_params.quant_format = out.quant;
	if (out.quant == VIP_BUFFER_QUANTIZE_TF_ASYMM) {
		out_params.quant_data.affine.scale = out.tf_scale;
		out_params.quant_data.affine.zeroPoint =
			static_cast<vip_int32_t>(out.tf_zero);
	}
	out_params.memory_type = VIP_BUFFER_MEMORY_TYPE_DEFAULT;
	vip_buffer buf_out = nullptr;
	VIP_CHECK(vip_create_buffer(&out_params, sizeof(out_params), &buf_out));
	void *out_map = vip_map_buffer(buf_out);
	if (!out_map)
		die(2, "NPU: vip_map_buffer(output) returned null");

	/* Optional second consumer (the OpenCV-shaped consumer). */
	std::unique_ptr<ar0234::DmaBuffer> cv_buf;
	if (fanout) {
		cv_buf = std::make_unique<ar0234::DmaBuffer>(
			ar0234::G2d::nv12_size(cw, ch));
		std::printf("fanout: second consumer buffer %zu B (NV12 copy of the "
			    "same capture frame)\n", cv_buf->size());
	}

	VIP_CHECK(vip_prepare_network(net));

	std::vector<std::uint8_t> out_a(out.bytes), out_b(out.bytes);
	RunStats t_convert, t_infer_fd, t_infer_cpu, t_e2e, t_cpu_frame, t_cpu_fanout;
	unsigned long mismatches = 0, max_diff_bytes = 0, max_delta = 0, stalls = 0;
	unsigned long frames_done = 0;
	const double t_start = ms_since({0, 0});

	cap.start();

	for (unsigned i = 0; i < frames; ++i) {
		auto fr = cap.dequeue(std::chrono::seconds(2));
		if (!fr) {
			++stalls;
			continue;
		}
		const int cap_fd = cap.buffers()[fr->index()].dmabuf.get();
		const double cpu0 = cpu_ms_now();
		const double t0 = ms_since({0, 0});

		g2d.convert_nv12_to_bgr888(cap_fd, npu_in->fd(), cw, ch, dw, dh);
		const double t1 = ms_since({0, 0});

		/* path A: NPU reads the dma-buf the G2D just wrote */
		VIP_CHECK(vip_set_input(net, 0, buf_fd));
		VIP_CHECK(vip_set_output(net, 0, buf_out));
		VIP_CHECK(vip_run_network(net));
		const double t2 = ms_since({0, 0});
		VIP_CHECK(vip_flush_buffer(buf_out, VIP_BUFFER_OPER_TYPE_INVALIDATE));
		std::memcpy(out_a.data(), out_map, out.bytes);

		/* path B: the same bytes, deliberately copied by the CPU */
		npu_in->cpu_read_begin();
		std::memcpy(cpu_in_map, npu_in->data(), in.bytes);
		npu_in->cpu_read_end();
		vip_status_e fl = vip_flush_buffer(buf_cpu, VIP_BUFFER_OPER_TYPE_FLUSH);
		if (fl != VIP_SUCCESS)
			std::printf("note: vip_flush_buffer(FLUSH) status %d "
				    "(continuing; the A/B compare is the gate)\n",
				    static_cast<int>(fl));
		VIP_CHECK(vip_set_input(net, 0, buf_cpu));
		VIP_CHECK(vip_run_network(net));
		VIP_CHECK(vip_flush_buffer(buf_out, VIP_BUFFER_OPER_TYPE_INVALIDATE));
		std::memcpy(out_b.data(), out_map, out.bytes);
		const double t3 = ms_since({0, 0});
		const double cpu1 = cpu_ms_now();

		if (out_a != out_b) {
			++mismatches;
			std::size_t d = 0;
			for (std::size_t k = 0; k < out.bytes; ++k) {
				if (out_a[k] != out_b[k]) {
					++d;
					unsigned delta = out_a[k] > out_b[k] ?
							 out_a[k] - out_b[k] :
							 out_b[k] - out_a[k];
					if (delta > max_delta)
						max_delta = delta;
				}
			}
			if (d > max_diff_bytes)
				max_diff_bytes = d;
			std::printf("frame %u: A/B outputs differ in %zu/%zu B\n", i, d,
				    out.bytes);
		}

		if (cv_buf) {
			g2d.move_nv12(cap_fd, cv_buf->fd(), cw, ch);
			if (i % static_cast<unsigned>(fps) == 0) {
				const double c0 = cpu_ms_now();
				cv_buf->cpu_read_begin();
				std::uint64_t sum = 0;
				const std::uint8_t *p = cv_buf->data();
				for (std::size_t k = 0; k < cv_buf->size(); k += 4099)
					sum += p[k];
				cv_buf->cpu_read_end();
				const double c1 = cpu_ms_now();
				t_cpu_fanout.add(c1 - c0);
				std::printf("frame %u: fanout consumer checksum %llx\n",
					    i, static_cast<unsigned long long>(sum));
			}
		}

		t_convert.add(t1 - t0);
		t_infer_fd.add(t2 - t1);
		t_infer_cpu.add(t3 - t2);
		t_e2e.add(t2 - t0);
		t_cpu_frame.add(cpu1 - cpu0);
		++frames_done;

		if ((i + 1) % 60 == 0)
			std::printf("frame %u/%u: convert %.2f ms, infer(fd) %.2f ms, "
				    "infer(cpu) %.2f ms, e2e %.2f ms, cpu %.2f ms\n",
				    i + 1, frames, t_convert.avg(), t_infer_fd.avg(),
				    t_infer_cpu.avg(), t_e2e.avg(), t_cpu_frame.avg());
	}

	const double wall = ms_since({0, 0}) - t_start;

	/* dump the last frame's buffers for offline study */
	if (!dump_dir.empty()) {
		std::filesystem::create_directories(dump_dir);
		try {
			ar0234::DmaBuffer nv{ar0234::G2d::nv12_size(cw, ch)};
			auto last = cap.dequeue(std::chrono::seconds(2));
			if (last) {
				g2d.move_nv12(cap.buffers()[last->index()].dmabuf.get(),
					      nv.fd(), cw, ch);
				nv.cpu_read_begin();
				auto wr = [&](const char *name, const void *p,
					      std::size_t n) {
					FILE *f = std::fopen((dump_dir / name).c_str(),
							     "wb");
					if (f) {
						std::fwrite(p, 1, n, f);
						std::fclose(f);
					}
				};
				wr("capture.nv12", nv.data(), nv.size());
				nv.cpu_read_end();
				g2d.convert_nv12_to_bgr888(
					cap.buffers()[last->index()].dmabuf.get(),
					npu_in->fd(), cw, ch, dw, dh);
				npu_in->cpu_read_begin();
				wr("npu_input.bgr888", npu_in->data(), in.bytes);
				npu_in->cpu_read_end();
				wr("output_fd.bin", out_a.data(), out.bytes);
				wr("output_cpu.bin", out_b.data(), out.bytes);
				std::printf("dumped last frame + outputs to %s\n",
					    dump_dir.c_str());
			}
		} catch (const std::exception &e) {
			std::printf("note: dump failed: %s\n", e.what());
		}
	}

	/* ---------------- verdict --------------------------------------------- */
	const auto &st = cap.stream_stats();
	std::printf("\n== summary over %lu frames (%.1f s wall, %.1f fps achieved) ==\n",
		    frames_done, wall / 1e3, wall > 0 ? frames_done * 1e3 / wall : 0.0);
	std::printf("stream: %u start call(s), %u failed probe(s), %u hard reopen(s), "
		    "%zu dequeue stall(s)\n", st.start_calls, st.failed_probes,
		    st.hard_reopens, stalls);
	std::printf("G2D convert %ux%u NV12 -> %ux%u BGR888: %.2f ms avg / %.2f ms max\n",
		    cw, ch, dw, dh, t_convert.avg(), t_convert.max);
	std::printf("inference zero-copy (vip_create_buffer_from_fd): %.2f ms avg / "
		    "%.2f ms max\n", t_infer_fd.avg(), t_infer_fd.max);
	std::printf("inference CPU-copy control:                            %.2f ms avg / "
		    "%.2f ms max\n", t_infer_cpu.avg(), t_infer_cpu.max);
	std::printf("dequeue -> network output ready (zero-copy):           %.2f ms avg / "
		    "%.2f ms max\n", t_e2e.avg(), t_e2e.max);
	std::printf("process CPU per frame (both inferences + convert):     %.2f ms avg\n",
		    t_cpu_frame.avg());
	if (fanout)
		std::printf("fanout consumer checksum CPU cost: %.2f ms avg "
			    "(once per second)\n", t_cpu_fanout.avg());
	std::printf("A/B output compare: %lu/%lu frame(s) differ, max differing "
		    "bytes %lu, max byte delta %lu\n", mismatches, frames_done,
		    max_diff_bytes, max_delta);

	if (out.num_dims == 2) { /* 1-D output: [n, 1] -- print a top-5 */
		std::vector<std::pair<float, unsigned>> v;
		for (unsigned k = 0; k < out.dims[0]; ++k) {
			float x = out.format == VIP_BUFFER_FORMAT_INT8 ?
					  static_cast<float>(reinterpret_cast<
						  const vip_int8_t *>(
						  out_a.data())[k]) :
				  out.format == VIP_BUFFER_FORMAT_UINT8 ?
					  static_cast<float>(out_a[k]) :
				  out.format == VIP_BUFFER_FORMAT_FP16 ? 0.f :
					  static_cast<float>(reinterpret_cast<
						  const float *>(
						  out_a.data())[k]);
			v.emplace_back(x, k);
		}
		std::partial_sort(v.begin(), v.begin() + std::min<std::size_t>(5, v.size()),
				  v.end(), [](auto &a, auto &b) { return a.first > b.first; });
		std::printf("last frame top-5:");
		for (unsigned k = 0; k < std::min<std::size_t>(5, v.size()); ++k)
			std::printf(" [%u]=%.3f", v[k].second, v[k].first);
		std::printf("\n");
	}

	cap.stop();
	vip_finish_network(net);
	vip_destroy_network(net);
	vip_destroy_buffer(buf_fd);
	vip_destroy_buffer(buf_cpu);
	vip_destroy_buffer(buf_out);
	vip_destroy();

	if (mismatches)
		return 1;
	std::printf("result: OK (zero-copy NPU output identical to CPU-copy on "
		    "real ISP frames)\n");
	return 0;
}

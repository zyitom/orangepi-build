// SPDX-License-Identifier: GPL-2.0-or-later
//
// ar0234-g2d-selftest -- worked example AND acceptance check for the narrow G2D
// wrapper in ar0234/g2d.hpp.
//
// It is written the way the project's other probes are: nothing is reported as
// "OK" until two self-controls have passed, because a broken comparison looks
// exactly like a broken blit.
//
//   control A  a constant plane must survive any correct data path  [must pass]
//   control B  a CPU memcpy through the same buffers and the same compare
//                                                [must pass, else exit 2]
//
// then the two operations the wrapper exposes:
//
//   phase 1  NV12 move, random full-range data (the worst case), two buffers
//   phase 2  NV12 move in place (src == dst): every byte must be unchanged
//   phase 3  NV12 move of a real ISP frame (--capture) -- the acceptance test
//   phase 4  rectangle annotation: nothing outside the rectangle may change,
//            everything inside must equal the requested (Y, U, V)
//
// USAGE
//   ar0234-g2d-selftest [--capture] [--w N] [--h N]
//     --capture  also pull one real frame from /dev/video0 and move that
//
// EXIT CODES
//   0  every required check passed
//   1  CHECK-FAIL: a measured-correct operation was not exact
//   2  CONTROL-FAIL: the harness itself is broken, all verdicts invalid
//   3  setup error (/dev/g2d, /dev/dma_heap/system, capture)
//
// BUILD (native on the board, from userspace/):
//   make -j8 && ./build/ar0234-g2d-selftest --capture

#include <ar0234/g2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string_view>
#include <vector>

#include <ar0234/v4l2.hpp>

namespace {

int g_rc = 0;

void check_fail(const char *what)
{
	std::printf("  %-34s <-- CHECK-FAIL\n", what);
	g_rc = 1;
}

/// Prints a plane comparison and, when `must_be_exact`, fails the run if it differs.
bool report(const char *what, const ar0234::DiffStat &s, bool must_be_exact = true)
{
	std::printf("  %-34s %-9s %zu/%zu differing", what, s.exact() ? "[EXACT]" : "[DIFFERS]",
		    s.differ, s.bytes);
	if (!s.exact())
		std::printf("  maxdelta %zu", s.max_delta);
	std::printf("\n");
	if (!s.exact() && must_be_exact)
		check_fail(what);
	return s.exact();
}

/* deterministic full-range noise: the worst case for any data path that filters
 * or clamps, and the case the old "constant fill round-trips" control missed */
std::uint32_t g_xs = 0x1234567u;
std::uint8_t rnd8()
{
	g_xs ^= g_xs << 13;
	g_xs ^= g_xs >> 17;
	g_xs ^= g_xs << 5;
	return static_cast<std::uint8_t>(g_xs >> 8);
}

void fill_random(ar0234::DmaBuffer &b, std::size_t bytes)
{
	b.cpu_write_begin();
	for (std::size_t i = 0; i < bytes; ++i)
		b.data()[i] = rnd8();
	b.cpu_write_end();
}

void fill_const(ar0234::DmaBuffer &b, std::size_t bytes, std::uint8_t v)
{
	b.cpu_write_begin();
	std::memset(b.data(), v, bytes);
	b.cpu_write_end();
}

struct Geometry {
	std::uint32_t w, h;
	std::size_t luma() const { return static_cast<std::size_t>(w) * h; }
	std::size_t chroma() const { return luma() / 2; }
	std::size_t total() const { return luma() + chroma(); }
};

/* ------------------------------------------------------------------ phase 4 */
bool annotate_check(const ar0234::G2d &g2d, ar0234::DmaBuffer &buf, const Geometry &g,
		    const ar0234::Rect &r, std::uint8_t y, std::uint8_t u, std::uint8_t v)
{
	const std::uint8_t poison = 0x5A;
	fill_const(buf, g.total(), poison);
	g2d.fill_rect_nv12(buf, g.w, g.h, r, y, u, v);

	/* the hardware halves the rectangle for the chroma plane itself */
	const std::uint32_t cx = r.x / 2, cy = r.y / 2, cw = r.w / 2, ch = r.h / 2;

	buf.cpu_read_begin();
	std::size_t in_y = 0, out_y = 0, in_c = 0, out_c = 0;
	for (std::uint32_t row = 0; row < g.h; ++row) {
		for (std::uint32_t col = 0; col < g.w; ++col) {
			const std::uint8_t px = buf.data()[static_cast<std::size_t>(row) * g.w + col];
			const bool inside = col >= r.x && col < r.x + r.w &&
					    row >= r.y && row < r.y + r.h;
			if (inside)
				in_y += px == y;
			else
				out_y += px != poison;
		}
	}
	for (std::uint32_t row = 0; row < g.h / 2; ++row) {
		for (std::uint32_t col = 0; col < g.w / 2; ++col) {
			const std::size_t off = g.luma() + (static_cast<std::size_t>(row) * g.w + col * 2);
			const std::uint8_t pu = buf.data()[off], pv = buf.data()[off + 1];
			const bool inside = col >= cx && col < cx + cw && row >= cy && row < cy + ch;
			if (inside)
				in_c += (pu == u && pv == v);
			else
				out_c += !(pu == poison && pv == poison);
		}
	}
	buf.cpu_read_end();

	const std::size_t want_y = static_cast<std::size_t>(r.w) * r.h;
	const std::size_t want_c = static_cast<std::size_t>(cw) * ch;
	std::printf("  rect (%u,%u,%ux%u) colour Y=%u U=%u V=%u\n", r.x, r.y, r.w, r.h, y, u, v);
	std::printf("    luma   inside %zu/%zu = colour, outside %zu changed (want 0)\n",
		    in_y, want_y, out_y);
	std::printf("    chroma inside %zu/%zu = colour, outside %zu changed (want 0)\n",
		    in_c, want_c, out_c);

	bool ok = in_y == want_y && out_y == 0 && in_c == want_c && out_c == 0;
	if (!ok)
		check_fail("fill_rect_nv12");
	return ok;
}

} // namespace

int main(int argc, char **argv)
{
	bool capture = false;
	std::uint32_t w = 1920, h = 1200;
	for (int i = 1; i < argc; ++i) {
		const std::string_view a{argv[i]};
		auto next = [&]() -> std::uint32_t {
			return i + 1 < argc ? static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0)) : 0;
		};
		if (a == "--capture")
			capture = true;
		else if (a == "--w")
			w = next();
		else if (a == "--h")
			h = next();
		else {
			std::fprintf(stderr, "usage: %s [--capture] [--w N] [--h N]\n", argv[0]);
			return 1;
		}
	}
	if (w == 0 || h == 0 || (h & 1) || (w & 1)) {
		std::fprintf(stderr, "width and height must be non-zero and even\n");
		return 1;
	}

	const Geometry g{w, h};
	std::printf("ar0234-g2d-selftest: %ux%u NV12 (%zu bytes), %zu luma + %zu chroma\n", w, h,
		    g.total(), g.luma(), g.chroma());

	try {
		ar0234::G2d g2d;
		ar0234::DmaBuffer src{g.total()};
		ar0234::DmaBuffer dst{g.total()};

		/* ---------------------------------------------------------- controls */
		std::printf("\n== self-controls (must pass, otherwise every number below is meaningless) ==\n");
		fill_const(src, g.total(), 0x80);
		dst.cpu_write_begin();
		std::memset(dst.data(), 0xA5, g.total());
		std::memcpy(dst.data(), src.data(), g.total());       /* the CPU reference */
		dst.cpu_write_end();
		dst.cpu_read_begin();
		auto ctrl_b = ar0234::diff_bytes(dst.data(), src.data(), g.total());
		dst.cpu_read_end();
		std::printf("  %-34s %s\n", "control B: CPU memcpy through the buffers",
			    ctrl_b.exact() ? "[EXACT] ok" : "[DIFFERS]");
		if (!ctrl_b.exact()) {
			std::printf("\nCONTROL-FAIL: the harness itself is broken\n");
			return 2;
		}

		g2d.move_nv12(src, dst, w, h);
		dst.cpu_read_begin();
		auto ctrl_a = ar0234::diff_bytes(dst.data(), src.data(), g.total());
		dst.cpu_read_end();
		std::printf("  %-34s %s\n", "control A: constant plane through G2D",
			    ctrl_a.exact() ? "[EXACT] ok" : "[DIFFERS]");
		if (!ctrl_a.exact()) {
			std::printf("\nCONTROL-FAIL: a constant plane must survive any correct path\n");
			return 2;
		}

		/* -------------------------------------------------- phase 1: exact move */
		std::printf("\n== phase 1: bit-exact NV12 move, random full-range data (worst case) ==\n");
		fill_random(src, g.total());
		fill_const(dst, g.total(), 0x00);
		g2d.move_nv12(src, dst, w, h);
		dst.cpu_read_begin();
		report("luma (Y)", ar0234::diff_bytes(dst.data(), src.data(), g.luma()));
		report("chroma (UV)", ar0234::diff_bytes(dst.data() + g.luma(),
							 src.data() + g.luma(), g.chroma()));
		dst.cpu_read_end();

		/* ------------------------------------------------------ phase 2: in place */
		std::printf("\n== phase 2: in-place move (src == dst) must change nothing ==\n");
		std::vector<std::uint8_t> snapshot(g.total());
		src.cpu_read_begin();
		std::memcpy(snapshot.data(), src.data(), g.total());
		src.cpu_read_end();
		g2d.move_nv12(src, src, w, h);            /* must not corrupt anything */
		g2d.fill_rect_nv12(src, w, h, {0, 0, 0, 0}, 0, 0, 0);   /* degenerate rect = no-op */
		src.cpu_read_begin();
		report("unchanged after in-place move", ar0234::diff_bytes(src.data(), snapshot.data(),
									  g.total()));
		src.cpu_read_end();

		/* ---------------------------------------------------- phase 4: annotation */
		std::printf("\n== phase 4: rectangle annotation on NV12 ==\n");
		annotate_check(g2d, dst, g, {100, 64, 256, 64}, 235, 128, 128);   /* white-ish */
		annotate_check(g2d, dst, g, {0, 0, 64, 64}, 76, 85, 255);         /* red-ish */
		annotate_check(g2d, dst, g, {w - 64, h - 64, 64, 64}, 29, 255, 107); /* clipped at edge */

		/* ------------------------------------------------------ phase 3: real frame */
		if (capture) {
			std::printf("\n== phase 3: real ISP frame (%ux%u) through the wrapper ==\n", w, h);
			ar0234::CaptureConfig cc{};
			cc.size = {w, h};
			cc.fps = 30;
			ar0234::Capture cap{cc};
			if (!cap.dmabuf_exported())
				throw std::runtime_error{"capture buffers are not DMA-BUF exported"};
			auto bufs = cap.buffers();
			cap.start();
			auto frame = cap.dequeue(std::chrono::seconds(3));
			if (!frame)
				throw std::runtime_error{"no frame within 3 s"};
			const auto &st = cap.stream_stats();
			std::printf("  stream: %u start(s), %u failed probe(s), %u reopen(s), seq %u\n",
				    st.start_calls, st.failed_probes, st.hard_reopens, frame->sequence());
			auto view = bufs[frame->index()].data();
			src.cpu_write_begin();
			std::memcpy(src.data(), view.data(), std::min(view.size(), g.total()));
			src.cpu_write_end();
			cap.stop();

			fill_const(dst, g.total(), 0x00);
			g2d.move_nv12(src, dst, w, h);
			dst.cpu_read_begin();
			report("real frame luma (Y)", ar0234::diff_bytes(dst.data(), src.data(), g.luma()));
			report("real frame chroma (UV)", ar0234::diff_bytes(dst.data() + g.luma(),
									   src.data() + g.luma(), g.chroma()));
			dst.cpu_read_end();

			/* Zero-copy source: move straight out of the ISP's own dma-buf
			 * (no CPU copy at all).  This is the real use case.  It is
			 * reported but not enforced: if the capture buffer had a padded
			 * pitch the wrapper's linear assumption would be wrong, and that
			 * would be a finding about the buffer layout, not about G2D. */
			if (bufs[frame->index()].dmabuf) {
				fill_const(dst, g.total(), 0x00);
				g2d.move_nv12(bufs[frame->index()].dmabuf.get(), dst.fd(), w, h);
				dst.cpu_read_begin();
				report("ISP dma-buf -> heap, luma",
				       ar0234::diff_bytes(dst.data(), src.data(), g.luma()), false);
				report("ISP dma-buf -> heap, chroma",
				       ar0234::diff_bytes(dst.data() + g.luma(), src.data() + g.luma(),
							  g.chroma()), false);
				dst.cpu_read_end();
			}

			annotate_check(g2d, dst, g, {32, 32, 128, 128}, 235, 128, 128);
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "SETUP-FAIL: %s\n", e.what());
		return 3;
	}

	std::printf("\n%s\n", g_rc == 0 ? "result: OK" : "result: CHECK-FAIL");
	return g_rc;
}

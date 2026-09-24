// G2D copy-correctness harness for sun60iw2 (A733, Orange Pi Zero 3W).
//
// WHY
//   /dev/g2d is how we intend to move / annotate the ISP's NV12 frames. This tool
//   answers, per plane and per channel, whether a G2D blit is bit-exact -- and it
//   refuses to hand out a G2D verdict unless two self-controls pass first.
//
// USAGE
//   g2d_test [--capture] [--w N] [--h N]
//     (no flags)  synthetic data only; no camera needed, runs as a plain user
//                 in the "video" group.
//     --capture   additionally pull one real ISP frame from /dev/video0 and run
//                 the same comparison on it. The camera must be free (do not run
//                 this while ar0234-3ad holds the sensor).
//
// EXIT CODES
//   0  controls and every documented-exact case passed
//   1  a documented-exact case failed (CHECK-FAIL)
//   2  a self-control failed (CONTROL-FAIL): the board or this harness is broken,
//      so every G2D verdict below it is meaningless
//   3  setup error (/dev/g2d, /dev/dma_heap/system)
//
// THREE THINGS THIS TOOL DOES THAT THE OLD VERSION GOT WRONG
//   1. alpha. ARGB data is written with alpha = 0xFF before the pixel compare,
//      because G2D premultiplies: with random alpha, the differing bytes are
//      exactly the alpha==0 pixels. Random alpha is used only by the explicitly
//      labelled premultiply probe, which is *expected* to differ.
//   2. DMA-BUF sync order on the CPU side:
//        write src:  SYNC_START, write, SYNC_END          (flush to device)
//        after blt:  SYNC_START, read, SYNC_END           (invalidate from device)
//      The old version called START *before* the ioctl and END *after* it, i.e.
//      the exact opposite, and could compare against stale cache lines. Worse,
//      it also hard-coded the *mainline* DMA_BUF_SYNC flag layout, which this
//      vendor 6.6 kernel rejects with EINVAL -- and with the return value
//      ignored, every sync was a silent no-op (analysis/round9/REPORT.md,
//      "DMA_BUF_IOCTL_SYNC 旧编码"). The layout is now negotiated at the first
//      sync (same scheme as ar0234::DmaBuffer in userspace/src/g2d.cpp) and any
//      sync failure is fatal (CONTROL-FAIL, exit 2), never ignored.
//   3. buffers. Two dedicated /dev/dma_heap/system buffers are src and dst. A
//      V4L2 capture buffer is never used as the G2D destination; with --capture
//      the frame is CPU-copied into the src heap buffer first.
//
// LIMITS / THINGS TO KNOW BEFORE TRUSTING A NUMBER
//   * fixed geometry, no stride or padding: align[] is left 0 so every pitch is
//     linear; only the requested w/h is supported.
//   * only G2D_BLT_NONE_H is exercised: G2D_BLT_COPYPEN returns -1 (EINVAL) on
//     this kernel. bbuff must be 1; with bbuff=0 the blit returns stale data.
//   * the NV12 chroma plane is *expected* to differ for non-constant data. The
//     chroma path is a normalised transition filter, not a byte move, so a
//     YUV420UVC -> YUV420UVC blit can never be bit-exact; a bit-exact NV12 copy
//     is instead done with two G2D_FORMAT_Y8 blits (phase 1b below). Full
//     evidence: analysis/g2d/REPORT.md.
//   * sample printing uses a 997-byte stride on purpose: 997 is prime and is not
//     a multiple of 256, so it cannot alias against a 256-byte-period pattern
//     (an earlier probe used a 256 stride against a 256-period ramp and every
//     sample landed on the same phase, which made the reading meaningless).
//   * the diff counts themselves are not sampled at all -- they scan the whole
//     plane, so no aliasing is possible there.
//
// FACTS THIS TOOL ENCODES (all measured, see analysis/g2d/REPORT.md)
//   * NV12_FMT below is G2D_FORMAT_YUV420UVC_V1U1V0U0 (0x28): that is the one
//     that puts U in the even byte, i.e. real V4L2 NV12. The sibling constant
//     G2D_FORMAT_YUV420UVC_U1V1U0V0 (0x29) is *NV21* despite its name; using it
//     on NV12 data still copies Y correctly but swaps red and blue in any
//     ARGB8888 -> NV12 or NV12 -> ARGB8888 conversion.
//   * annotate NV12 with G2D_CMD_FILLRECT_H, colour = dst_image_h.color =
//     (Y<<16)|(U<<8)|V, written straight through with no RGB->YUV matrix.
//
// BUILD (native on the board, from userspace/):
//   g++ -O2 -std=c++20 -Iinclude -o /tmp/g2d_test ../tools/hwtest/g2d_test.cpp src/v4l2.cpp -lpthread
// (the aarch64 cross toolchain on TL101 is glibc 2.34 while the board is glibc
// 2.31, so build on the board)

#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>

#ifndef G2D_WITH_CAPTURE
#define G2D_WITH_CAPTURE 1     /* 0 = build without the V4L2/ISP capture phase */
#endif

#if G2D_WITH_CAPTURE
#include <ar0234/v4l2.hpp>
#endif

extern "C" {
#include "sunxi-g2d.h"
}

/* ---- dma-heap / dma-buf definitions (linux/dma-heap.h is absent here) ------- */
struct dma_heap_allocation_data {
	__u64 len;
	__u32 fd;
	__u32 fd_flags;
	__u64 heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

struct dma_buf_sync {
	__u64 flags;
};
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

/* The flag *layout* is negotiated on the first sync call. This vendor 6.6
 * kernel still uses the legacy encoding (READ = 1<<0, WRITE = 2<<0, RW = 3,
 * START = 0<<2, END = 1<<2, valid mask = 7); mainline 6.12+ moved READ/WRITE
 * to bits 2/3 and END to bit 0. The two layouts are mutually exclusive (legacy
 * RW = 3 is outside the mainline mask, mainline RW = 12 is outside the legacy
 * mask), so the kernel can only ever accept one of them. */
struct SyncEncoding {
	std::uint64_t read, write, end;
};
static const SyncEncoding kLegacySync{1ull << 0, 2ull << 0, 1ull << 2};
static const SyncEncoding kMainlineSync{1ull << 2, 2ull << 2, 1ull << 0};
static const SyncEncoding *g_sync_encoding = nullptr;

static std::uint64_t sync_flags(const SyncEncoding &e, bool end, bool write)
{
	/* the direction is mandatory: the kernel rejects 0 and END-alone */
	return (end ? e.end : 0) | (write ? e.write : e.read);
}

/* Throws on failure -- a silently no-op sync is exactly the bug this tool
 * once had, so any error here aborts the run as CONTROL-FAIL (exit 2). */
static void dmabuf_sync(int fd, bool end, bool write)
{
	if (!g_sync_encoding) {
		for (const SyncEncoding *enc : {&kLegacySync, &kMainlineSync}) {
			dma_buf_sync s{sync_flags(*enc, end, write)};
			if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0) {
				g_sync_encoding = enc;
				return;
			}
			if (errno != EINVAL)
				break;
		}
		throw std::runtime_error(
			"DMA_BUF_IOCTL_SYNC rejected both known flag encodings");
	}
	dma_buf_sync s{sync_flags(*g_sync_encoding, end, write)};
	if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) < 0)
		throw std::runtime_error{std::string{"DMA_BUF_IOCTL_SYNC: "} +
					 strerror(errno)};
}

static constexpr std::size_t kNoIdx = ~std::size_t{0};
static constexpr std::size_t kProbeStride = 997;   /* prime, not a multiple of 256 */

/* ---------------------------------------------------------------- heap buffer */
struct HeapBuf {
	int fd = -1;
	std::size_t len = 0;
	std::uint8_t *map = nullptr;

	bool alloc(std::size_t n)
	{
		len = n;
		int h = open("/dev/dma_heap/system", O_RDONLY);
		if (h < 0)
			return false;
		dma_heap_allocation_data d{};
		d.len = n;
		d.fd_flags = O_RDWR | O_CLOEXEC;
		if (ioctl(h, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
			close(h);
			return false;
		}
		close(h);
		fd = static_cast<int>(d.fd);
		map = static_cast<std::uint8_t *>(
			mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
		return map != MAP_FAILED;
	}
	/* START = begin CPU access (invalidate on read, nothing flushed yet),
	 * END   = finish CPU access (flush written lines to the device) */
	void sync(bool end, bool write) const { dmabuf_sync(fd, end, write); }
	void cpu_write_begin() const { sync(false, true); }
	void cpu_write_end() const { sync(true, true); }
	void cpu_read_begin() const { sync(false, false); }
	void cpu_read_end() const { sync(true, false); }
	~HeapBuf()
	{
		if (map && map != MAP_FAILED)
			munmap(map, len);
		if (fd >= 0)
			close(fd);
	}
};

/* ------------------------------------------------------------------- compare */
struct Stat {
	std::size_t n = 0, diff = 0, first = kNoIdx;
	unsigned maxd = 0;
	std::size_t hist[4] = {0, 0, 0, 0};   /* ==0, <=2, <=8, >8 */
	std::size_t exact() const { return hist[0]; }
};

static Stat compare(const std::uint8_t *dst, const std::uint8_t *src,
		    std::size_t base, std::size_t len)
{
	Stat s;
	s.n = len;
	for (std::size_t i = 0; i < len; ++i) {
		unsigned a = dst[base + i], b = src[base + i];
		if (a == b) {
			++s.hist[0];
			continue;
		}
		++s.diff;
		if (s.first == kNoIdx)
			s.first = i;
		unsigned d = a > b ? a - b : b - a;
		if (d > s.maxd)
			s.maxd = d;
		++s.hist[d <= 2 ? 1 : d <= 8 ? 2 : 3];
	}
	return s;
}

static bool report(const char *name, const Stat &s, bool must_be_exact)
{
	bool ok = s.diff == 0;
	printf("  %-22s %-9s exact %zu/%zu  (<=2 %zu, <=8 %zu, >8 %zu)  maxdelta %u",
	       name, ok ? "[EXACT]" : "[DIFFERS]", s.exact(), s.n,
	       s.hist[1], s.hist[2], s.hist[3], s.maxd);
	if (!ok)
		printf("  first@+%zu", s.first);
	if (!ok && must_be_exact)
		printf("   <-- CHECK-FAIL");
	printf("\n");
	return ok || !must_be_exact;
}

/* per-row-band spread: shows whether the damage is uniform or concentrated */
static void band_report(const char *name, const std::uint8_t *dst, const std::uint8_t *src,
			std::size_t base, std::size_t len, unsigned bands)
{
	std::size_t per = len / bands;
	printf("  %s: per-band differing bytes (band = %zu B):", name, per);
	for (unsigned b = 0; b < bands; ++b) {
		std::size_t lo = b * per, hi = (b + 1 == bands) ? len : (b + 1) * per;
		std::size_t d = 0;
		for (std::size_t i = lo; i < hi; ++i)
			d += dst[base + i] != src[base + i];
		printf(" %zu", d);
	}
	printf("\n");
}

/* ---------------------------------------------------------------- G2D blit */
/* 0x28 = G2D_FORMAT_YUV420UVC_V1U1V0U0 = U in the even byte = real V4L2 NV12.
 * 0x29 (G2D_FORMAT_YUV420UVC_U1V1U0V0) is NV21 despite its name. */
static constexpr g2d_fmt_enh NV12_FMT = G2D_FORMAT_YUV420UVC_V1U1V0U0;

static int do_blt(int g2d, int sfd, int dfd, g2d_fmt_enh fmt, unsigned w, unsigned h)
{
	g2d_blt_h b{};
	b.flag_h = G2D_BLT_NONE_H;
	b.src_image_h.fd = sfd;
	b.src_image_h.format = fmt;
	b.src_image_h.width = w;
	b.src_image_h.height = h;
	b.src_image_h.clip_rect.x = 0;
	b.src_image_h.clip_rect.y = 0;
	b.src_image_h.clip_rect.w = w;
	b.src_image_h.clip_rect.h = h;
	b.src_image_h.bbuff = 1;          /* must be 1: bbuff=0 returns stale data */
	b.src_image_h.use_phy_addr = 0;   /* fd path, driver computes laddr[] */
	b.dst_image_h = b.src_image_h;
	b.dst_image_h.fd = dfd;
	return ioctl(g2d, G2D_CMD_BITBLT_H, &b);
}

/* Bit-exact NV12 copy: two Y8 blits over the same buffer viewed as w x (h + h/2)
 * 8-bit rows. clip_rect.y selects the luma plane (0, h) or the chroma plane
 * (h, h/2) in place. A Y8 blit is bit-exact, keeps full range and applies no
 * clamp, which the YUV420UVC blit does not. */
static int do_y8_blt(int g2d, int sfd, int dfd, unsigned w, unsigned rows,
		     unsigned y0, unsigned hh)
{
	g2d_blt_h b{};
	b.flag_h = G2D_BLT_NONE_H;
	b.src_image_h.fd = sfd;
	b.src_image_h.format = G2D_FORMAT_Y8;
	b.src_image_h.width = w;
	b.src_image_h.height = rows;
	b.src_image_h.clip_rect.x = 0;
	b.src_image_h.clip_rect.y = y0;
	b.src_image_h.clip_rect.w = w;
	b.src_image_h.clip_rect.h = hh;
	b.src_image_h.bbuff = 1;
	b.src_image_h.use_phy_addr = 0;
	b.dst_image_h = b.src_image_h;
	b.dst_image_h.fd = dfd;
	return ioctl(g2d, G2D_CMD_BITBLT_H, &b);
}

/* runs one complete blt with the correct CPU-side sync discipline and returns
 * the comparison of dst against src */
struct BltResult {
	int rc = 0;
	Stat y, uv;
};

static BltResult run_blt(int g2d, HeapBuf &src, HeapBuf &dst, g2d_fmt_enh fmt,
			 unsigned w, unsigned h, std::size_t ylen, std::size_t uvlen)
{
	BltResult r;
	r.rc = do_blt(g2d, src.fd, dst.fd, fmt, w, h);   /* caller flushed both buffers */
	dst.cpu_read_begin();             /* invalidate: G2D wrote this buffer */
	r.y = compare(dst.map, src.map, 0, ylen);
	r.uv = compare(dst.map, src.map, ylen, uvlen);
	dst.cpu_read_end();
	return r;
}

/* --------------------------------------------------------- deterministic data */
static std::uint32_t xs = 0x1234567u;
static std::uint8_t rnd8()
{
	xs ^= xs << 13;
	xs ^= xs >> 17;
	xs ^= xs << 5;
	return static_cast<std::uint8_t>(xs >> 8);
}

static void fill_nv12(HeapBuf &b, unsigned w, unsigned h, unsigned mode)
{
	/* mode 0 = full range, 1 = studio-range safe (Y 16..235, UV 16..240),
	 * 2 = constant */
	std::size_t ylen = std::size_t{w} * h, n = ylen * 3 / 2;
	b.cpu_write_begin();
	for (std::size_t i = 0; i < ylen; ++i)
		b.map[i] = mode == 2 ? 0x10 : (mode == 1 ? 16 + rnd8() % 220 : rnd8());
	for (std::size_t i = ylen; i < n; ++i)
		b.map[i] = mode == 2 ? 0x80 : (mode == 1 ? 16 + rnd8() % 225 : rnd8());
	b.cpu_write_end();
}

static void fill_argb(HeapBuf &b, std::size_t n, bool opaque_alpha)
{
	b.cpu_write_begin();
	for (std::size_t i = 0; i + 3 < n; i += 4) {
		/* little-endian ARGB8888 => bytes are B,G,R,A */
		b.map[i + 0] = rnd8();
		b.map[i + 1] = rnd8();
		b.map[i + 2] = rnd8();
		b.map[i + 3] = opaque_alpha ? 0xFF : rnd8();
	}
	b.cpu_write_end();
}

static void poison(HeapBuf &b)
{
	b.cpu_write_begin();
	memset(b.map, 0xA5, b.len);
	b.cpu_write_end();
}

int main(int argc, char **argv)
{
	unsigned w = 1920, h = 1080;
	bool capture = false;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--capture"))
			capture = true;
		else if (!strcmp(argv[i], "--w") && i + 1 < argc)
			w = static_cast<unsigned>(atoi(argv[++i]));
		else if (!strcmp(argv[i], "--h") && i + 1 < argc)
			h = static_cast<unsigned>(atoi(argv[++i]));
		else {
			printf("usage: %s [--capture] [--w N] [--h N]\n", argv[0]);
			return 3;
		}
	}
	if (w & 1 || h & 1) {
		printf("FAIL: w and h must be even for NV12\n");
		return 3;
	}

	/* every phase below depends on the sync discipline working; any failure
	 * of it is a harness problem and invalidates every verdict */
	int rc = 0;
	try {
	int g2d = open("/dev/g2d", O_RDWR);
	if (g2d < 0) {
		printf("FAIL: open /dev/g2d: %s (is g2d_sunxi loaded?)\n", strerror(errno));
		return 3;
	}
	g2d_hardware_version ver{};
	if (ioctl(g2d, G2D_CMD_QUERY_VERSION, &ver) == 0)
		printf("g2d version 0x%llx, %ux%u\n", (unsigned long long)ver.g2d_version, w, h);
	else
		printf("QUERY_VERSION failed: %s\n", strerror(errno));

	const std::size_t ylen = std::size_t{w} * h;
	const std::size_t nv12 = ylen * 3 / 2;
	const std::size_t uvlen = nv12 - ylen;
	const std::size_t argblen = std::size_t{w} * h * 4;

	HeapBuf nsrc, ndst, asrc, adst;
	if (!nsrc.alloc(nv12) || !ndst.alloc(nv12) || !asrc.alloc(argblen) || !adst.alloc(argblen)) {
		printf("FAIL: dma_heap alloc/mmap: %s\n", strerror(errno));
		return 3;
	}

	/* ---------------- phase 0: self-controls (mandatory) ------------------ */
	printf("\n== phase 0: self-controls ==\n");
	{
		/* control 1: constant fill must round-trip bit-exact */
		fill_nv12(nsrc, w, h, 2);
		poison(ndst);
		BltResult r = run_blt(g2d, nsrc, ndst, NV12_FMT,
				      w, h, ylen, uvlen);
		if (r.rc < 0) {
			printf("CONTROL-FAIL: blt rc=%d (%s)\n", r.rc, strerror(errno));
			return 2;
		}
		bool a = report("ctrl1 const Y", r.y, true);
		bool b = report("ctrl1 const UV", r.uv, true);
		if (!a || !b) {
			printf("CONTROL-FAIL: constant fill is not bit-exact -> board or harness broken;\n"
			       "              ignore every G2D verdict below.\n");
			return 2;
		}
	}
	{
		/* control 2: CPU memcpy of the *complex* data must round-trip exactly,
		 * which validates the buffers, the sync discipline and the compare */
		fill_nv12(nsrc, w, h, 0);
		poison(ndst);
		ndst.cpu_write_begin();
		memcpy(ndst.map, nsrc.map, nv12);
		ndst.cpu_write_end();
		ndst.cpu_read_begin();
		Stat y = compare(ndst.map, nsrc.map, 0, ylen);
		Stat uv = compare(ndst.map, nsrc.map, ylen, uvlen);
		ndst.cpu_read_end();
		bool a = report("ctrl2 memcpy Y", y, true);
		bool b = report("ctrl2 memcpy UV", uv, true);
		if (!a || !b) {
			printf("CONTROL-FAIL: CPU memcpy is not bit-exact -> harness/board broken;\n"
			       "              ignore every G2D verdict below.\n");
			return 2;
		}
	}

	/* ------------- phase 1: NV12 data-dependence (the real probe) --------- */
	printf("\n== phase 1: NV12 -> NV12, G2D_FORMAT_YUV420UVC_V1U1V0U0 (0x28) ==\n");
	{
		/* full-range pseudo-random data: no period of 256 to alias against */
		fill_nv12(nsrc, w, h, 0);
		poison(ndst);
		BltResult r = run_blt(g2d, nsrc, ndst, NV12_FMT,
				      w, h, ylen, uvlen);
		if (r.rc < 0) {
			printf("FAIL: blt rc=%d (%s)\n", r.rc, strerror(errno));
			return 1;
		}
		report("full-range Y", r.y, false);
		report("full-range UV", r.uv, false);
		band_report("full-range UV", ndst.map, nsrc.map, ylen, uvlen, 8);

		/* studio-range data: isolates "out of range values got clamped" from
		 * "the plane geometry is wrong" */
		fill_nv12(nsrc, w, h, 1);
		poison(ndst);
		r = run_blt(g2d, nsrc, ndst, NV12_FMT, w, h, ylen, uvlen);
		printf("  (Y kept in 16..235, UV in 16..240 -- no value can be range-clamped)\n");
		report("in-range Y", r.y, true);
		report("in-range UV", r.uv, false);
		band_report("in-range UV", ndst.map, nsrc.map, ylen, uvlen, 8);

		/* sparse sample table with a prime stride: printed so the byte-level
		 * relationship can be read off without any 256-period aliasing */
		printf("  sample (stride %zu, prime; nv12=Y[0..%zu)+UV[%zu..%zu)):\n",
		       kProbeStride, ylen, ylen, nv12);
		int shown = 0;
		for (std::size_t i = 0; i < nv12 && shown < 10; i += kProbeStride) {
			if (i < ylen || (i - ylen) < 4096 || (i - ylen) > uvlen - 4096) {
				printf("    plane %s off %7zu  dst %02x  src %02x  %s\n",
				       i < ylen ? "Y" : "UV", i < ylen ? i : i - ylen,
				       ndst.map[i], nsrc.map[i],
				       ndst.map[i] == nsrc.map[i] ? "=" : "!=");
				++shown;
			}
		}
	}

	/* ------ phase 1b: the bit-exact NV12 path (two Y8 blits) -------------- */
	printf("\n== phase 1b: NV12 -> NV12 as two G2D_FORMAT_Y8 blits (bit-exact path) ==\n");
	{
		/* worst case on purpose: full-range random, nothing can be clamped */
		fill_nv12(nsrc, w, h, 0);
		poison(ndst);
		int r1 = do_y8_blt(g2d, nsrc.fd, ndst.fd, w, h + h / 2, 0, h);
		int r2 = do_y8_blt(g2d, nsrc.fd, ndst.fd, w, h + h / 2, h, h / 2);
		ndst.cpu_read_begin();
		Stat y = compare(ndst.map, nsrc.map, 0, ylen);
		Stat uv = compare(ndst.map, nsrc.map, ylen, uvlen);
		ndst.cpu_read_end();
		bool a = report("y8-pair Y", y, true);
		bool b = report("y8-pair UV", uv, true);
		printf("  blit rc=%d (luma), %d (chroma); this is the supported way to move\n"
		       "  an NV12 frame with G2D -- the YUV420UVC blit above can never be exact.\n",
		       r1, r2);
		if (!a || !b) {
			printf("CHECK-FAIL: the documented bit-exact path is not bit-exact\n");
			rc = 1;
		}
	}

	/* ------------------------- phase 2: ARGB8888 ------------------------- */
	printf("\n== phase 2: ARGB8888 (bytes in memory are B,G,R,A) ==\n");
	{
		fill_argb(asrc, argblen, true);
		poison(adst);
		int r = do_blt(g2d, asrc.fd, adst.fd, G2D_FORMAT_ARGB8888, w, h);
		if (r < 0) {
			printf("FAIL: blt rc=%d (%s)\n", r, strerror(errno));
			rc = 1;
		} else {
			adst.cpu_read_begin();
			const char *nm[4] = {"ch0(B)", "ch1(G)", "ch2(R)", "ch3(A)"};
			bool allok = true;
			for (int c = 0; c < 4; ++c) {
				Stat s;
				s.n = argblen / 4;
				for (std::size_t i = 0; i < argblen; i += 4) {
					unsigned a = adst.map[i + c], b = asrc.map[i + c];
					if (a == b) {
						++s.hist[0];
						continue;
					}
					++s.diff;
					if (s.first == kNoIdx)
						s.first = i / 4;
					unsigned d = a > b ? a - b : b - a;
					if (d > s.maxd)
						s.maxd = d;
					++s.hist[d <= 2 ? 1 : d <= 8 ? 2 : 3];
				}
				allok &= report(nm[c], s, true);
			}
			adst.cpu_read_end();
			printf("  alpha=0xFF enforced: %s\n",
			       allok ? "G2D is bit-exact for ARGB8888" : "NOT bit-exact");
			if (!allok)
				rc = 1;
		}

		/* premultiply probe: expected to change. G2D premultiplies RGB by A,
		 * so a source pixel with alpha==0 comes back as RGB==0. */
		fill_argb(asrc, argblen, false);
		poison(adst);
		if (do_blt(g2d, asrc.fd, adst.fd, G2D_FORMAT_ARGB8888, w, h) < 0) {
			printf("  premul probe: blt failed: %s\n", strerror(errno));
		} else {
			adst.cpu_read_begin();
			Stat s;
			s.n = argblen / 4;
			std::size_t alpha0 = 0, alpha0_diff = 0;
			for (std::size_t i = 0; i < argblen; i += 4) {
				if (asrc.map[i + 3] == 0) {
					++alpha0;
					alpha0_diff += memcmp(&adst.map[i], &asrc.map[i], 3) != 0;
				}
				for (int c = 0; c < 4; ++c) {
					if (adst.map[i + c] == asrc.map[i + c]) {
						++s.hist[0];
						continue;
					}
					++s.diff;
					unsigned d = adst.map[i + c] > asrc.map[i + c]
							     ? adst.map[i + c] - asrc.map[i + c]
							     : asrc.map[i + c] - adst.map[i + c];
					if (d > s.maxd)
						s.maxd = d;
					++s.hist[d <= 2 ? 1 : d <= 8 ? 2 : 3];
				}
			}
			adst.cpu_read_end();
			printf("  premul probe [expected to differ]: differing bytes %zu/%zu;"
			       " pixels with alpha==0: %zu/%zu had RGB changed\n",
			       s.diff, s.n, alpha0_diff, alpha0);
			puts("  ^ this is documented premultiply behaviour, NOT a failure");
		}
	}

	/* ---------------------- phase 3: real ISP frame ---------------------- */
	if (capture) {
		printf("\n== phase 3: real ISP NV12 frame -> G2D -> NV12 ==\n");
#if G2D_WITH_CAPTURE
		try {
			ar0234::CaptureConfig cc{};
			cc.size = {w, h};
			cc.fps = 30;
			ar0234::Capture cap{cc};
			if (!cap.dmabuf_exported())
				throw std::runtime_error("no dmabuf export");
			auto bufs = cap.buffers();
			cap.start();
			auto fr = cap.dequeue(std::chrono::seconds(2));
			if (!fr)
				throw std::runtime_error("no frame");
			const auto &st = cap.stream_stats();
			printf("  stream: %u start(s), %u failed probe(s), %u STREAMON failure(s),"
			       " %u reopen(s), seq %u\n",
			       st.start_calls, st.failed_probes, st.streamon_failures,
			       st.hard_reopens, fr->sequence());
			/* copy out of the V4L2 buffer into our own heap src; the capture
			 * buffer is never a G2D destination */
			auto s = bufs[fr->index()].data();
			nsrc.cpu_write_begin();
			memcpy(nsrc.map, s.data(), std::min(s.size(), nv12));
			nsrc.cpu_write_end();
			poison(ndst);
			BltResult r = run_blt(g2d, nsrc, ndst, NV12_FMT,
					      w, h, ylen, uvlen);
			/* the capture must be stopped before any of this can race */
			cap.stop();
			if (r.rc < 0) {
				printf("FAIL: blt rc=%d (%s)\n", r.rc, strerror(errno));
				rc = 1;
			} else {
				report("isp frame Y (yuv420)", r.y, false);
				report("isp frame UV (yuv420)", r.uv, false);
				band_report("isp frame UV", ndst.map, nsrc.map, ylen, uvlen, 8);
			}
			/* the supported path: two Y8 blits, must be bit-exact on real frames */
			poison(ndst);
			do_y8_blt(g2d, nsrc.fd, ndst.fd, w, h + h / 2, 0, h);
			do_y8_blt(g2d, nsrc.fd, ndst.fd, w, h + h / 2, h, h / 2);
			ndst.cpu_read_begin();
			Stat y8 = compare(ndst.map, nsrc.map, 0, ylen);
			Stat u8 = compare(ndst.map, nsrc.map, ylen, uvlen);
			ndst.cpu_read_end();
			bool a = report("isp frame Y (y8 pair)", y8, true);
			bool b = report("isp frame UV (y8 pair)", u8, true);
			if (!a || !b)
				rc = 1;
		} catch (const std::exception &e) {
			printf("FAIL: capture: %s\n", e.what());
			rc = 1;
		}
#else
		printf("  built without capture support (G2D_WITH_CAPTURE=0)\n");
#endif
	}

	} catch (const std::exception &e) {
		printf("CONTROL-FAIL: %s\n", e.what());
		return 2;
	}

	printf("\nresult: %s\n", rc == 0 ? "OK" : "CHECK-FAIL");
	return rc;
}

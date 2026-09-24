// SPDX-License-Identifier: GPL-2.0-or-later
#include "ar0234/encoder.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <memoryAdapter.h>
#include <veAdapter.h>
#include <veInterface.h>
#include <vencoder.h>
}

namespace ar0234 {

namespace {

[[nodiscard]] constexpr std::uint32_t align16(std::uint32_t v) noexcept
{
	return (v + 15) & ~15u;
}

} // namespace

struct VideoEncoder::Impl {
	EncoderConfig cfg;
	ScMemOpsS *memops = nullptr;
	::VideoEncoder *enc = nullptr;
	bool initialised = false;
	bool allocated = false;
	std::vector<user_iommu_param> iova; // index = capture buffer index
	std::vector<std::span<std::uint8_t>> mapped;

	~Impl()
	{
		if (enc) {
			for (auto &p : iova)
				VideoEncoderFreeVeIommuAddr(enc, &p);
			if (allocated)
				ReleaseAllocInputBuffer(enc);
			if (initialised)
				VideoEncUnInit(enc);
			VideoEncDestroy(enc);
		}
		if (memops)
			CdcMemClose(memops);
	}

	void configure()
	{
		const int fps = cfg.fps;
		if (cfg.codec == Codec::H264) {
			VencH264Param p{};
			p.sProfileLevel.nProfile = VENC_H264ProfileHigh;
			p.sProfileLevel.nLevel = VENC_H264Level51;
			p.bEntropyCodingCABAC = 1;
			p.sQPRange.nMinqp = 10;
			p.sQPRange.nMaxqp = 45;
			p.nFramerate = fps;
			p.nSrcFramerate = fps;
			p.nBitrate = cfg.bitrate;
			p.nMaxKeyInterval = fps;
			p.nCodingMode = VENC_FRAME_CODING;
			VideoEncSetParameter(enc, VENC_IndexParamH264Param, &p);
		} else {
			VencH265Param p{};
			p.sProfileLevel.nProfile = VENC_H265ProfileMain;
			p.sProfileLevel.nLevel = VENC_H265Level51;
			p.sQPRange.nMinqp = 10;
			p.sQPRange.nMaxqp = 45;
			p.nFramerate = fps;
			p.nSrcFramerate = fps;
			p.nBitrate = cfg.bitrate;
			p.idr_period = fps;
			p.nIntraPeriod = fps;
			// cedarc rejects gop_size above 63 and silently falls back to 2
			p.nGopSize = std::min(fps, 60);
			p.nQPInit = 30;
			VideoEncSetParameter(enc, VENC_IndexParamH265Param, &p);
		}
	}

	void drain(const BitstreamSink &sink)
	{
		VencOutputBuffer out{};
		while (GetOneBitstreamFrame(enc, &out) == 0) {
			sink(std::span<const std::uint8_t>{out.pData0, static_cast<std::size_t>(out.nSize0)});
			if (out.nSize1)
				sink(std::span<const std::uint8_t>{out.pData1, static_cast<std::size_t>(out.nSize1)});
			FreeOneBitStreamFrame(enc, &out);
		}
	}

	void encode_imported(unsigned index, std::int64_t pts, const BitstreamSink &sink)
	{
		const std::uint32_t luma = cfg.size.width * cfg.size.height;
		VencInputBuffer in{};
		in.nID = index;
		in.nPts = pts;
		in.pAddrVirY = mapped[index].data();
		in.pAddrVirC = mapped[index].data() + luma;
		in.pAddrPhyY = reinterpret_cast<unsigned char *>(static_cast<unsigned long>(iova[index].iommu_addr));
		in.pAddrPhyC = reinterpret_cast<unsigned char *>(static_cast<unsigned long>(iova[index].iommu_addr + luma));
		// With a share fd VE re-derives the chroma address from the 16-aligned
		// height (1088) and the bottom rows turn green; vin puts UV right after
		// the w*h luma plane.
		in.nShareBufFd = -1;
		AddOneInputBuffer(enc, &in);
		VideoEncodeOneFrame(enc);
		AlreadyUsedInputBuffer(enc, &in);
		drain(sink);
	}

	void encode_copy(std::span<const std::uint8_t> src, std::int64_t pts, const BitstreamSink &sink)
	{
		const std::uint32_t w = cfg.size.width, h = cfg.size.height;
		const std::uint32_t aw = align16(w), ah = align16(h);
		if (!allocated) {
			VencAllocateBufferParam ap{};
			ap.nBufferNum = 4;
			ap.nSizeY = aw * ah;
			ap.nSizeC = aw * ah / 2;
			if (AllocInputBuffer(enc, &ap))
				throw std::runtime_error{"AllocInputBuffer failed"};
			allocated = true;
		}
		VencInputBuffer in{};
		if (GetOneAllocInputBuffer(enc, &in))
			return;

		// VE reads whole 16-aligned planes: replicate the last row/column into the padding
		for (std::uint32_t y = 0; y < ah; ++y) {
			auto *d = in.pAddrVirY + y * aw;
			std::memcpy(d, src.data() + std::min(y, h - 1) * w, w);
			std::fill(d + w, d + aw, d[w - 1]);
		}
		for (std::uint32_t y = 0; y < ah / 2; ++y) {
			auto *d = in.pAddrVirC + y * aw;
			std::memcpy(d, src.data() + w * h + std::min(y, h / 2 - 1) * w, w);
			for (std::uint32_t x = w; x + 1 < aw; x += 2) {
				d[x] = d[w - 2];
				d[x + 1] = d[w - 1];
			}
		}
		in.nPts = pts;
		FlushCacheAllocInputBuffer(enc, &in);
		AddOneInputBuffer(enc, &in);
		VideoEncodeOneFrame(enc);
		AlreadyUsedInputBuffer(enc, &in);
		ReturnOneAllocInputBuffer(enc, &in);
		drain(sink);
	}
};

VideoEncoder::VideoEncoder(const EncoderConfig &cfg) : impl_{std::make_unique<Impl>()}
{
	auto &d = *impl_;
	d.cfg = cfg;
	d.memops = MemAdapterGetOpsS();
	if (!d.memops || CdcMemOpen(d.memops) < 0) {
		d.memops = nullptr;
		throw std::runtime_error{"cedarc memory adapter open failed"};
	}

	// like libOmxVenc: VideoEncInit() creates and owns the VE instance
	VencBaseConfig base{};
	base.memops = d.memops;
	base.nInputWidth = cfg.size.width;
	base.nInputHeight = cfg.size.height;
	base.nStride = cfg.size.width;
	base.nDstWidth = cfg.size.width;
	base.nDstHeight = cfg.size.height;
	base.eInputFormat = VENC_PIXEL_YUV420SP;

	d.enc = VideoEncCreate(cfg.codec == Codec::H264 ? VENC_CODEC_H264 : VENC_CODEC_H265);
	if (!d.enc)
		throw std::runtime_error{"VideoEncCreate failed"};
	d.configure();
	if (VideoEncInit(d.enc, &base))
		throw std::runtime_error{"VideoEncInit failed"};
	d.initialised = true;
}

VideoEncoder::~VideoEncoder() = default;

std::vector<std::uint8_t> VideoEncoder::stream_header()
{
	VencHeaderData hdr{};
	const auto index = impl_->cfg.codec == Codec::H264 ? VENC_IndexParamH264SPSPPS
							    : VENC_IndexParamH265Header;
	if (VideoEncGetParameter(impl_->enc, index, &hdr) || !hdr.pBuffer)
		return {};
	return {hdr.pBuffer, hdr.pBuffer + hdr.nLength};
}

bool VideoEncoder::import_buffers(std::span<CaptureBuffer> buffers)
{
	auto &d = *impl_;
	std::vector<user_iommu_param> iova;
	std::vector<std::span<std::uint8_t>> mapped;
	for (auto &b : buffers) {
		user_iommu_param p{};
		p.fd = b.dmabuf.get();
		if (p.fd >= 0)
			VideoEncoderGetVeIommuAddr(d.enc, &p);
		if (!p.iommu_addr) {
			for (auto &q : iova)
				VideoEncoderFreeVeIommuAddr(d.enc, &q);
			return false;
		}
		iova.push_back(p);
		mapped.push_back(b.data());
	}
	d.iova = std::move(iova);
	d.mapped = std::move(mapped);
	return !d.iova.empty();
}

bool VideoEncoder::zero_copy() const noexcept
{
	return !impl_->iova.empty();
}

void VideoEncoder::encode(unsigned index, std::span<const std::uint8_t> nv12, std::int64_t pts_us,
			  const BitstreamSink &sink)
{
	if (zero_copy() && index < impl_->iova.size())
		impl_->encode_imported(index, pts_us, sink);
	else
		impl_->encode_copy(nv12, pts_us, sink);
}

} // namespace ar0234

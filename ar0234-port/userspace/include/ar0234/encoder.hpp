// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "ar0234/v4l2.hpp"

namespace ar0234 {

enum class Codec { H264, H265 };

[[nodiscard]] constexpr std::string_view to_string(Codec c) noexcept
{
	return c == Codec::H264 ? "H.264" : "H.265";
}

struct EncoderConfig {
	Resolution size;
	int fps = 30;
	int bitrate = 8'000'000;
	Codec codec = Codec::H264;
};

/// Receives each encoded bitstream chunk (Annex B).
using BitstreamSink = std::function<void(std::span<const std::uint8_t>)>;

/// Allwinner cedar VE hardware encoder for NV12 input.
class VideoEncoder {
public:
	explicit VideoEncoder(const EncoderConfig &cfg);
	VideoEncoder(const VideoEncoder &) = delete;
	VideoEncoder &operator=(const VideoEncoder &) = delete;
	~VideoEncoder();

	/// SPS/PPS (and VPS for H.265) to put in front of the stream.
	[[nodiscard]] std::vector<std::uint8_t> stream_header();

	/// Map capture DMA-BUFs into the VE IOMMU so frames are encoded in place.
	/// Returns false (and stays in copy mode) if any buffer cannot be mapped.
	[[nodiscard]] bool import_buffers(std::span<CaptureBuffer> buffers);
	[[nodiscard]] bool zero_copy() const noexcept;

	/// Encode capture buffer `index` (imported) or copy `nv12` into a VE buffer.
	void encode(unsigned index, std::span<const std::uint8_t> nv12, std::int64_t pts_us,
		    const BitstreamSink &sink);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace ar0234

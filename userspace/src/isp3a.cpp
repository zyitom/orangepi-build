// SPDX-License-Identifier: GPL-2.0-or-later
#include "ar0234/isp3a.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <AWIspApi.h>

namespace ar0234 {

namespace fs = std::filesystem;

struct Isp3A::Session {
	struct ApiDeleter {
		void operator()(AWIspApi *api) const noexcept { DestroyAWIspApi(api); }
	};
	std::unique_ptr<AWIspApi, ApiDeleter> api;
	bool initialised = false;
	int isp_id = -1;

	~Session()
	{
		if (isp_id >= 0) {
			api->ispStop(isp_id);
			api->ispWaitToExit(isp_id);
		}
		if (initialised)
			api->ispApiUnInit();
	}
};

Isp3A::Isp3A(int video_id, int fps) : session_{std::make_unique<Session>()}
{
	auto &s = *session_;
	s.api.reset(CreateAWIspApi());
	if (!s.api || s.api->ispApiInit() < 0)
		throw std::runtime_error{"libisp init failed"};
	s.initialised = true;

	const int id = s.api->ispGetIspId(video_id);
	if (id < 0 || s.api->ispStart(id) < 0)
		throw std::runtime_error{"libisp start failed for video" + std::to_string(video_id)};
	s.isp_id = id;
	if (fps > 0)
		s.api->ispSetFpsRanage(id, fps);
}

Isp3A::~Isp3A() = default;

int Isp3A::isp_id() const noexcept
{
	return session_->isp_id;
}

namespace {

std::vector<char> read_all(const fs::path &path)
{
	std::ifstream in{path, std::ios::binary};
	return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

std::optional<fs::path> IspParamSets::select(Resolution sensor, int fps,
					     const std::string &force) const
{
	const char *name = nullptr;
	if (!force.empty()) {
		name = force.c_str(); // /etc/ar0234.conf params= override
	} else {
		name = needs_3dnr_off(sensor, fps) ? "no3dnr" : "3dnr";
	}
	const fs::path source = dir / ("isp_param_" + std::string{name} + ".bin");
	const auto wanted = read_all(source);
	if (wanted.empty() || wanted == read_all(active))
		return std::nullopt;

	fs::path tmp = active;
	tmp += ".tmp";
	{
		std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
		out.write(wanted.data(), static_cast<std::streamsize>(wanted.size()));
		if (!out.flush())
			throw std::runtime_error{"write " + tmp.string()};
	}
	fs::rename(tmp, active);
	return source;
}

// libisp parameter files are [u32 size][20 B date/time][50 B note][struct];
// every offset in this function is a STRUCT offset, the file adds 74.
constexpr std::size_t kParamHeader = 74;

void put_u32(std::vector<char> &buf, std::size_t struct_off, std::uint32_t v)
{
	std::memcpy(buf.data() + kParamHeader + struct_off, &v, sizeof v);
}

void put_u8(std::vector<char> &buf, std::size_t struct_off, std::uint8_t v)
{
	buf[kParamHeader + struct_off] = static_cast<char>(v);
}

std::optional<fs::path> apply_fixed_mode(const FixedParams &fp,
					 const fs::path &active)
{
	auto buf = read_all(active);
	if (buf.size() < kParamHeader + 200)
		throw std::runtime_error{"parameter file too small: " + active.string()};
	std::uint32_t declared = 0;
	std::memcpy(&declared, buf.data(), sizeof declared);
	if (declared != buf.size() - kParamHeader)
		throw std::runtime_error{"parameter file header size mismatch: " +
					 active.string()};

	put_u8(buf, 88, 1); // manual_en = 1 (ae_en = awb_en = 0 below)
	put_u8(buf, 90, 0); // ae_en = 0
	put_u8(buf, 92, 0); // awb_en = 0
	if (fp.gain_16)
		put_u32(buf, 68, *fp.gain_16); // isp_gain
	if (fp.exp_val_16)
		put_u32(buf, 72, *fp.exp_val_16); // isp_exp_line
	if (fp.color_temp)
		put_u32(buf, 76, *fp.color_temp); // isp_color_temp
	if (fp.ae_log)
		put_u32(buf, 64, 0x3); // isp_log_param: EXP_TIME/AGAIN/WB logs
	if (buf == read_all(active))
		return std::nullopt;

	fs::path tmp = active;
	tmp += ".tmp";
	{
		std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
		out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
		if (!out.flush())
			throw std::runtime_error{"write " + tmp.string()};
	}
	fs::rename(tmp, active);
	return active;
}

} // namespace ar0234

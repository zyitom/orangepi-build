// SPDX-License-Identifier: GPL-2.0-or-later
#include "ar0234/isp3a.hpp"

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

std::optional<fs::path> IspParamSets::select(Resolution sensor, int fps) const
{
	const fs::path source = dir / (needs_3dnr_off(sensor, fps) ? "isp_param_no3dnr.bin"
								    : "isp_param_3dnr.bin");
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

} // namespace ar0234

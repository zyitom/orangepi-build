// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include "ar0234/v4l2.hpp"

namespace ar0234 {

/// One libisp 3A (AE/AWB) session for a vin video node.
///
/// libisp only initialises once the sensor pipeline is bound, i.e. while a
/// capture is configured or streaming. Keep libisp and cedarc out of the same
/// process: both export iniparser/dictionary symbols with different layouts.
class Isp3A {
public:
	/// fps > 0 caps the AE exposure to one frame period (ispSetFpsRanage).
	Isp3A(int video_id, int fps);
	Isp3A(const Isp3A &) = delete;
	Isp3A &operator=(const Isp3A &) = delete;
	~Isp3A();

	[[nodiscard]] int isp_id() const noexcept;

private:
	struct Session; // holds the vendor AWIspApi handle
	std::unique_ptr<Session> session_;
};

/// Parameter sets libisp can run with; it only reads /mnt/extsd/isp_param_config.bin.
struct IspParamSets {
	std::filesystem::path dir = "/mnt/extsd/ar0234";
	std::filesystem::path active = "/mnt/extsd/isp_param_config.bin";

	/// The ISP 3DNR needs vertical blanking to update its reference frames and
	/// loses every frame at 1920x1200@120 (16 blank lines).
	[[nodiscard]] static bool needs_3dnr_off(Resolution sensor, int fps) noexcept
	{
		return sensor.height >= 1200 && fps > 110;
	}

	/// Installs the matching set atomically if it differs from the active file.
	/// Returns the installed source, nullopt if nothing changed or no set exists.
	std::optional<std::filesystem::path> select(Resolution sensor, int fps) const;
};

} // namespace ar0234

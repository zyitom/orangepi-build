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
	/// loses every frame at 1920x1200@120 (16 blank lines). The kernel side
	/// (patches/0006) force-disables it whenever the sensor vblank is below
	/// 500 us. Installing the 3DNR-on parameter set against that state makes
	/// libisp fight the interlock and the stream dies in a sunxi_isp_reset
	/// storm with zero frames produced (measured 2026-09-17 at 1080p >= 133
	/// fps: 4 of 5 runs storm with the daemon up, the same request runs
	/// 133.4 fps / 0 lost with the daemon stopped, and 5 of 5 runs are clean
	/// once this rule installs the no-3DNR set). Sensor constants from
	/// ar0234_mipi.c: pclk 90 MHz, hts 612 -> one line = 6.8 us, FLL overhead
	/// 5 lines, per-mode vts minimum.
	[[nodiscard]] static bool needs_3dnr_off(Resolution sensor, int fps) noexcept
	{
		if (fps <= 110)
			return false;
		if (sensor.height >= 1200)
			return true;
		const std::uint32_t vts_min = sensor.height >= 1080 ? 1096 :
					      sensor.height >= 720  ? 736 :
								      620;
		unsigned fll = 90'000'000u / (612u * static_cast<unsigned>(fps));
		fll = fll > 5 ? fll - 5 : 0;
		if (fll < vts_min)
			fll = vts_min;
		const unsigned vblank_us =
			(fll > sensor.height ? fll - sensor.height : 0) * 68 / 10;
		return vblank_us < 500;
	}

	/// Installs the matching set atomically if it differs from the active file.
	/// A non-empty `force` (3dnr | no3dnr | industrial, from /etc/ar0234.conf
	/// "params=") replaces the automatic choice. Returns the installed source,
	/// nullopt if nothing changed or no set exists.
	std::optional<std::filesystem::path> select(Resolution sensor, int fps,
						    const std::string &force = {}) const;
};

/// Values for the fixed-exposure/gain/AWB mode from /etc/ar0234.conf, written
/// into the active ISP parameter file before libisp starts (NEXT-TASKS C14).
/// Units are the driver's sensor_s_exp_gain units (ar0234_mipi.c): gain in
/// 1/16-x, exposure in 1/16-line. A nullopt field leaves the parameter file's
/// value untouched.
struct FixedParams {
	std::optional<std::uint32_t> gain_16;
	std::optional<std::uint32_t> exp_val_16;
	std::optional<std::uint32_t> color_temp; ///< kelvin, when AWB is pinned
	bool ae_log = false;                     ///< isp_log_param = 0x3
	bool gamma_linear = false;               ///< straight-line 0..4095 gamma tables
};

/// Patches the active parameter file (struct offsets: isp_gain 68, isp_exp_line
/// 72, isp_color_temp 76, isp_log_param 64, manual_en 88, ae_en 90, awb_en 92;
/// the file adds a 74-byte header). Atomic; returns the patched path, nullopt
/// if the file already had these values. Throws on I/O errors.
[[nodiscard]] std::optional<std::filesystem::path>
apply_fixed_mode(const FixedParams &fp, const std::filesystem::path &active);

} // namespace ar0234

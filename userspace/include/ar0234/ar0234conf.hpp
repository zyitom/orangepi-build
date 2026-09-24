// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// /etc/ar0234.conf -- the industrial fixed-mode configuration for the AR0234
// pipeline (NEXT-TASKS C14). Parsed by ar0234-3ad and applied to the ISP
// parameter file before libisp starts, so the mode holds for ANY program that
// streams (v4l2-ctl, OpenCV, the in-house tools), not just ar0234-rec.
//
// Why a parameter file and not V4L2 controls: the ISP sub-device does expose
// exposure/gain/WB controls, but on this kernel their only consumer
// (CONFIG_ISP_SERVER_MELIS forwarding in bsp/drivers/vin/vin-isp/sunxi_isp.c,
// __sunxi_isp_ctrl) is NOT SET in the kernel config, so a control write changes
// nothing outside the control cache. The parameter file is read by libisp at
// every stream start; patching it is the only path that exists end to end.
//
// File format (key = value, '#' or ';' comments, blank lines ignored):
//
//   mode = auto | fixed            fixed pins exposure/gain/AWB
//   params = 3dnr|no3dnr|industrial  force the parameter set, overriding the
//                                  automatic 1920x1200@120 3DNR choice
//   exposure_lines = N             sensor lines (the driver's native unit;
//                                  one line is 6.8 us on every AR0234 mode:
//                                  hts 612 / pclk 90 MHz, ar0234_mipi.c)
//   exposure_us = N                convenience alias, converted with the
//                                  6.8 us line time; give at most one of the two
//   gain = X                       sensor gain, x (1.0 .. 255.9)
//   awb = auto | fixed
//   wb_temperature = K             colour temperature preset for fixed AWB
//   ae_log = 0|1                   set isp_log_param=0x3 in the parameter file:
//                                  libisp logs EXP_TIME/AGAIN/WB Gain lines to
//                                  the ar0234-3ad unit journal while it runs
//   gamma = linear                 overwrite the ISP gamma tables with a straight
//                                  line 0..4095 (5 rows x 3ch x 1024 points, the
//                                  same table tools/make_isp_bin.py --gamma-table
//                                  linear writes). Works in auto mode too; the
//                                  sensor's default curve lifts shadows, the
//                                  linear one does not -- measure, don't assume.
//
// A missing file, or mode=auto, keeps the previous behaviour exactly.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ar0234 {

class Ar0234Conf {
public:
	/// Parse `path`; a missing file yields the auto/default config (present()
	/// false). Throws std::runtime_error on an unparseable existing file.
	[[nodiscard]] static Ar0234Conf load(const std::filesystem::path &path =
						     "/etc/ar0234.conf");

	[[nodiscard]] bool present() const noexcept { return present_; }
	[[nodiscard]] bool fixed() const noexcept { return fixed_; }
	[[nodiscard]] bool ae_log() const noexcept { return ae_log_; }
	[[nodiscard]] bool gamma_linear() const noexcept { return gamma_linear_; }

	/// Forced parameter-set name ("" = automatic 3dnr/no3dnr choice).
	[[nodiscard]] const std::string &params() const noexcept { return params_; }

	/// Exposure in the driver's 1/16-line units (sensor_s_exp_gain exp_val),
	/// from exposure_lines or exposure_us. nullopt when not configured.
	[[nodiscard]] std::optional<std::uint32_t> exp_val_16() const;

	/// Gain in the driver's 1/16-x units (sensor_s_exp_gain gain_val,
	/// clamped 1.0x .. 255.9x there). nullopt when not configured.
	[[nodiscard]] std::optional<std::uint32_t> gain_16() const;

	/// Colour temperature for fixed AWB; nullopt when awb = auto.
	[[nodiscard]] std::optional<std::uint32_t> wb_temperature() const;

	/// Parse problems (unknown keys, bad values) found by load(); empty for a
	/// missing file. ar0234-3ad logs them once at start-up.
	[[nodiscard]] const std::vector<std::string> &warnings() const noexcept
	{
		return warnings_;
	}

	/// One line of "key = value" suitable for a warning message.
	[[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
	bool present_ = false;
	bool fixed_ = false;
	bool ae_log_ = false;
	bool gamma_linear_ = false;
	std::string params_;
	std::optional<std::uint32_t> exp_val_16_;
	std::optional<std::uint32_t> gain_16_;
	std::optional<std::uint32_t> wb_temperature_;
	std::vector<std::string> warnings_;
};

} // namespace ar0234

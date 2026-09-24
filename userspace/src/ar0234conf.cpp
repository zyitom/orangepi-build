// SPDX-License-Identifier: GPL-2.0-or-later
#include "ar0234/ar0234conf.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include <cerrno>
#include <cstring>

namespace ar0234 {
namespace {

// Every AR0234 mode in ar0234_mipi.c runs hts = 612 at pclk = 90 MHz, so the
// line time is constant and the exposure_us convenience key can be converted
// exactly: one line = 612 / 90e6 s = 6.8 us.
constexpr double kLineUs = 6.8;

std::string trim(const std::string &s)
{
	std::size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	std::size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

bool parse_number(const std::string &s, double &out)
{
	if (s.empty())
		return false;
	char *end = nullptr;
	errno = 0;
	out = std::strtod(s.c_str(), &end);
	return end == s.c_str() + s.size() && errno == 0 && out >= 0;
}

std::string where(const std::filesystem::path &p, unsigned lineno)
{
	return p.string() + ":" + std::to_string(lineno) + ": ";
}

} // namespace

Ar0234Conf Ar0234Conf::load(const std::filesystem::path &path)
{
	Ar0234Conf conf;
	conf.path_ = path;

	std::ifstream in{path};
	if (!in)
		return conf; // missing file: the documented default behaviour
	conf.present_ = true;

	bool fixed_awb = false;
	std::optional<double> wb_temp, exp_lines, exp_us, gain;
	bool have_lines = false, have_us = false;

	std::string line;
	unsigned lineno = 0;
	while (std::getline(in, line)) {
		++lineno;
		const auto hash = line.find_first_of("#;");
		if (hash != std::string::npos)
			line = line.substr(0, hash);
		line = trim(line);
		if (line.empty())
			continue;

		const auto eq = line.find('=');
		if (eq == std::string::npos) {
			conf.warnings_.push_back(
				where(path, lineno) + "not key=value: " + line);
			continue;
		}
		const std::string key = trim(line.substr(0, eq));
		const std::string value = trim(line.substr(eq + 1));
		double num = 0;
		const bool numeric = parse_number(value, num);

		if (key == "mode") {
			if (value == "fixed")
				conf.fixed_ = true;
			else if (value != "auto")
				conf.warnings_.push_back(where(path, lineno) +
							 "mode must be auto|fixed, got " +
							 value);
		} else if (key == "params") {
			if (value == "3dnr" || value == "no3dnr" || value == "industrial")
				conf.params_ = value;
			else
				conf.warnings_.push_back(where(path, lineno) +
							 "params must be 3dnr|no3dnr|industrial, got " +
							 value);
		} else if (key == "exposure_lines") {
			if (!numeric || num <= 0)
				conf.warnings_.push_back(
					where(path, lineno) +
					"exposure_lines wants a positive number");
			else {
				exp_lines = num;
				have_lines = true;
			}
		} else if (key == "exposure_us") {
			if (!numeric || num <= 0)
				conf.warnings_.push_back(
					where(path, lineno) +
					"exposure_us wants a positive number");
			else {
				exp_us = num;
				have_us = true;
			}
		} else if (key == "gain") {
			if (!numeric || num <= 0)
				conf.warnings_.push_back(where(path, lineno) +
							 "gain wants a positive number");
			else
				gain = num;
		} else if (key == "awb") {
			if (value == "fixed")
				fixed_awb = true;
			else if (value != "auto")
				conf.warnings_.push_back(where(path, lineno) +
							 "awb must be auto|fixed, got " +
							 value);
		} else if (key == "wb_temperature") {
			if (!numeric || num <= 0)
				conf.warnings_.push_back(
					where(path, lineno) +
					"wb_temperature wants a positive number (kelvin)");
			else
				wb_temp = num;
		} else if (key == "ae_log") {
			conf.ae_log_ = value == "1" || value == "true";
		} else if (key == "gamma") {
			if (value == "linear")
				conf.gamma_linear_ = true;
			else if (value == "default")
				conf.gamma_linear_ = false;
			else
				conf.warnings_.push_back(where(path, lineno) +
							 "gamma must be linear|default, got " +
							 value);
		} else {
			conf.warnings_.push_back(where(path, lineno) +
						 "unknown key: " + key);
		}
	}

	if (have_lines && have_us)
		conf.warnings_.push_back(
			path.string() +
			": give exposure_lines or exposure_us, not both "
			"(exposure_lines wins)");

	if (exp_lines)
		conf.exp_val_16_ = static_cast<std::uint32_t>(*exp_lines * 16.0 + 0.5);
	else if (exp_us)
		conf.exp_val_16_ = static_cast<std::uint32_t>(*exp_us * 16.0 / kLineUs + 0.5);

	if (gain)
		conf.gain_16_ = static_cast<std::uint32_t>(*gain * 16.0 + 0.5);

	if (fixed_awb && wb_temp)
		conf.wb_temperature_ = static_cast<std::uint32_t>(*wb_temp + 0.5);
	else if (fixed_awb)
		conf.warnings_.push_back(path.string() +
					 ": awb=fixed without wb_temperature: AWB will be "
					 "pinned to the parameter file's preset gains");
	else if (wb_temp)
		conf.warnings_.push_back(path.string() +
					 ": wb_temperature without awb=fixed has no effect");

	return conf;
}

std::optional<std::uint32_t> Ar0234Conf::exp_val_16() const
{
	return exp_val_16_;
}

std::optional<std::uint32_t> Ar0234Conf::gain_16() const
{
	if (!gain_16_)
		return std::nullopt;
	// the driver clamps 1*16 .. 256*16 - 1 (sensor_s_exp_gain); clamp here so
	// the value written to the parameter file is the value the sensor gets
	return std::clamp(*gain_16_, 16u, 4095u);
}

std::optional<std::uint32_t> Ar0234Conf::wb_temperature() const
{
	return wb_temperature_;
}

} // namespace ar0234

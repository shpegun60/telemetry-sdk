/*
 * @file Meter.hpp
 * @brief The metrology of the example device: its own Config and a short spectrum.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Written without knowing the ADC: its Config has the same short name as
 * adc::Config. Like Adc.hpp it knows nothing of telemetry.
 */

#ifndef TELEMETRY_EXAMPLES_GROUP_TREE_METER_HPP
#define TELEMETRY_EXAMPLES_GROUP_TREE_METER_HPP
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace metrology {

enum class Window : std::uint8_t {
	Rectangular,
	Hann,
	FlatTop
};

struct Config {
	std::uint32_t cycles;
	Window window;
};

// The magnitudes of the first harmonics; one type per length.
template<std::size_t Orders>
struct Spectrum {
	std::array<float, Orders> magnitude;
};

class Meter {
public:
	Config config() const noexcept
	{
		return config_;
	}

	bool setConfig(const Config& value) noexcept
	{
		if (value.cycles == 0 || value.cycles > 64)
			return false;
		config_ = value;
		return true;
	}

	Spectrum<4> spectrum() const noexcept
	{
		return {{230.0f, 1.5f, 0.0f, 0.25f}};
	}

private:
	Config config_{10, Window::Hann};
};

} // namespace metrology

#endif

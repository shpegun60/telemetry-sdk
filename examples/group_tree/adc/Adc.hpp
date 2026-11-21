/*
 * @file Adc.hpp
 * @brief The ADC of the example device: two channel gains, a configuration and a status.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * An ordinary application class. It knows nothing of telemetry: no SDK
 * header, no names, no IDs. AdcTelemetry.hpp publishes it.
 */

#ifndef TELEMETRY_EXAMPLES_GROUP_TREE_ADC_HPP
#define TELEMETRY_EXAMPLES_GROUP_TREE_ADC_HPP
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace adc {

enum class Mode : std::uint8_t {
	Idle,
	Continuous,
	Triggered
};

struct Config {
	std::uint16_t rateHz;
	Mode mode;
	bool enabled;
};

struct Status {
	std::uint32_t samples;
	std::array<std::int16_t, 2> raw;
};

// The reference a calibration measures against, and the offset it finds.
struct Reference {
	float volts;
};

struct Offset {
	float counts;
};

class Adc {
public:
	template<std::size_t Channel>
	float gain() const noexcept
	{
		return gains_[Channel];
	}

	// Accepts gains 0 < g <= 16; the caller learns whether it applied.
	template<std::size_t Channel>
	bool setGain(float value) noexcept
	{
		if (!(value > 0.0f && value <= 16.0f))
			return false;
		gains_[Channel] = value;
		return true;
	}

	Config config() const noexcept
	{
		return config_;
	}

	bool setConfig(const Config& value) noexcept
	{
		if (value.rateHz == 0)
			return false;
		config_ = value;
		return true;
	}

	Status status() const noexcept
	{
		return {samples_, {-3, 7}};
	}

	void reset() noexcept
	{
		samples_ = 0;
		gains_ = {1.0f, 1.0f};
	}

	Offset calibrate(float volts) noexcept
	{
		samples_ += 64;
		return {volts * 2.0f};
	}

private:
	std::array<float, 2> gains_{1.0f, 1.0f};
	Config config_{1000, Mode::Continuous, true};
	std::uint32_t samples_ = 4096;
};

} // namespace adc

#endif

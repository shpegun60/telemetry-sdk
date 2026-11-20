/*
 * @file AdcBranch.hpp
 * @brief One branch file of the group-tree fixture: the ADC, its channels and its Config.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The device class knows nothing of telemetry; this file is the whole
 * telemetry side of the branch: one group expression, no tables, no IDs.
 * Its Config shares a short name with metrology::Config; the automatic type
 * names adc::Config and metrology::Config keep them apart.
 */

#ifndef TELEMETRY_TESTS_STRUCTURED_GROUP_TREE_ADC_BRANCH_HPP
#define TELEMETRY_TESTS_STRUCTURED_GROUP_TREE_ADC_BRANCH_HPP
#pragma once

#include <telemetry/Telemetry.hpp>

#include <array>
#include <cstdint>

namespace adc {

struct Config {
	float gain;
	std::uint16_t rateHz;
	bool enabled;
};

struct Status {
	std::uint16_t flags;
	std::array<std::int16_t, 2> raw;
};

struct Reference {
	float volts;
};

struct Offset {
	float counts;
};

// Counted device state; the getters and setters are plain member functions.
// API: gain/setGain per channel, config/setConfig, status, reset, calibrate.
class Adc {
public:
	template<std::size_t Channel>
	float gain() const noexcept
	{
		return gains_[Channel];
	}

	template<std::size_t Channel>
	telemetry::WriteResult setGain(const float& value) noexcept
	{
		gains_[Channel] = value;
		++writes;
		return telemetry::WriteResult::Applied;
	}

	Config config() const noexcept
	{
		return config_;
	}

	telemetry::WriteResult setConfig(const Config& value) noexcept
	{
		config_ = value;
		++writes;
		return telemetry::WriteResult::Applied;
	}

	Status status() const noexcept
	{
		return {0x0102, {-3, 7}};
	}

	telemetry::CommandResult reset() noexcept
	{
		++resets;
		return telemetry::CommandResult::Executed;
	}

	Offset calibrate(const Reference& reference) noexcept
	{
		++calibrations;
		return {reference.volts * 2.0f};
	}

	int writes = 0;
	int resets = 0;
	int calibrations = 0;

private:
	std::array<float, 2> gains_{1.5f, 2.5f};
	Config config_{0.75f, 1000, true};
};

} // namespace adc

namespace adc::tlm {
namespace ts = telemetry;

inline Adc instance;

inline constexpr auto branch = ts::group(
    "adc", ts::group("channel1", ts::field<&Adc::gain<0>, &Adc::setGain<0>>("Gain", instance)),
    ts::group("channel2", ts::field<&Adc::gain<1>, &Adc::setGain<1>>("Gain", instance)),
    ts::field<&Adc::config, &Adc::setConfig>("Config", instance),
    ts::field<&Adc::status>("Status", instance), ts::command<&Adc::reset>("Reset", instance),
    ts::service<&Adc::calibrate>("Calibrate", instance));

} // namespace adc::tlm

#endif

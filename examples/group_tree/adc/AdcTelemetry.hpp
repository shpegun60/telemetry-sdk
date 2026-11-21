/*
 * @file AdcTelemetry.hpp
 * @brief The ADC branch: everything telemetry knows about the ADC, in one file.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * One group expression with no tables, no IDs and no counting: two channel
 * subgroups beside a Config Field, a Status Field, a Reset Command and a
 * Calibrate Service. The small functions above it adapt the plain Adc class
 * to the signatures an endpoint takes. The type names are automatic:
 * adc::Config, adc::Status, adc::Mode, adc::Reference, adc::Offset.
 */

#ifndef TELEMETRY_EXAMPLES_GROUP_TREE_ADC_TELEMETRY_HPP
#define TELEMETRY_EXAMPLES_GROUP_TREE_ADC_TELEMETRY_HPP
#pragma once

#include "Adc.hpp"

#include <telemetry/Telemetry.hpp>

#include <cstddef>

namespace adc::tlm {
namespace ts = telemetry;

// The ADC this branch publishes; the application uses the same object.
inline Adc instance;

template<std::size_t Channel>
float gain() noexcept
{
	return instance.gain<Channel>();
}

template<std::size_t Channel>
ts::WriteResult setGain(const float& value) noexcept
{
	return instance.setGain<Channel>(value) ? ts::WriteResult::Applied
	                                        : ts::WriteResult::InvalidValue;
}

inline Config config() noexcept
{
	return instance.config();
}

inline ts::WriteResult setConfig(const Config& value) noexcept
{
	return instance.setConfig(value) ? ts::WriteResult::Applied : ts::WriteResult::InvalidValue;
}

inline Status status() noexcept
{
	return instance.status();
}

inline ts::CommandResult reset() noexcept
{
	instance.reset();
	return ts::CommandResult::Executed;
}

inline Offset calibrate(const Reference& reference) noexcept
{
	return instance.calibrate(reference.volts);
}

inline constexpr auto branch =
    ts::group("adc", ts::group("channel1", ts::field<&gain<0>, &setGain<0>>("Gain")),
              ts::group("channel2", ts::field<&gain<1>, &setGain<1>>("Gain")),
              ts::field<&config, &setConfig>("Config"), ts::field<&status>("Status"),
              ts::command<&reset>("Reset"), ts::service<&calibrate>("Calibrate"));

} // namespace adc::tlm

#endif

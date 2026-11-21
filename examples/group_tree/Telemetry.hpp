/*
 * @file Telemetry.hpp
 * @brief The one composition point: the branches mounted in one Model, and named handles.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Every branch file comes first, so every TypeReflection precedes the first
 * descriptor. Adding a branch is one include and one name in the Model; no
 * ID is written by hand. The handles below resolve their paths here, once:
 * a path that names nothing fails the build at its own line.
 */

#ifndef TELEMETRY_EXAMPLES_GROUP_TREE_TELEMETRY_HPP
#define TELEMETRY_EXAMPLES_GROUP_TREE_TELEMETRY_HPP
#pragma once

#include "adc/AdcTelemetry.hpp"
#include "metrology/MetrologyTelemetry.hpp"

#include <telemetry/descriptor/Descriptor.hpp>

namespace device {

inline constexpr telemetry::Model model{adc::tlm::branch, metrology::tlm::branch};
// Descriptor v3.1 with the group tree and the type names.
inline constexpr telemetry::descriptor::Descriptor descriptor{model};

// Named handles: path checked here, then gain1.read(), gain1.write(2.5f), reset.call().
inline constexpr auto gain1 = telemetry::endpoint<model, "adc/channel1/Gain">;
inline constexpr auto gain2 = telemetry::endpoint<model, "adc/channel2/Gain">;
inline constexpr auto adcConfig = telemetry::endpoint<model, "adc/Config">;
inline constexpr auto adcMode = telemetry::endpoint<model, "adc/Config.mode">; // reads only
inline constexpr auto reset = telemetry::endpoint<model, "adc/Reset">;
inline constexpr auto calibrate = telemetry::endpoint<model, "adc/Calibrate">;
inline constexpr auto meterConfig = telemetry::endpoint<model, "metrology/Config">;

} // namespace device

#endif

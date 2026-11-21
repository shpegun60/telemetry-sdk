// Generated from a validated Telemetry Descriptor v3. Do not edit.
// Descriptor SHA-256: f244f765841ad74cce0b79fc2f06ca20fc605bf47a0e1f8b942e24e97a3d53e6
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>

namespace device_api {
namespace detail {
using Type_0 = void;
using Type_1 = bool;
using Type_2 = std::uint8_t;
using Type_4 = std::uint16_t;
using Type_10 = float;
using Type_11 = double;
namespace fixture {
enum class Mode : Type_2 {
    Off = 0ULL,
    Run = 1ULL,
    Fault = 2ULL,
};
} // namespace fixture
using Type_12 = fixture::Mode;
using Type_13 = std::array<Type_4, 3>;
namespace fixture {
struct State {
    Type_10 volts;
    Type_1 active;
};
} // namespace fixture
using Type_14 = fixture::State;
namespace fixture {
struct MotorConfig {
    Type_10 target;
    Type_4 rpm;
    Type_1 enabled;
};
} // namespace fixture
using Type_15 = fixture::MotorConfig;
} // namespace detail

namespace motor::types {
using Enabled = detail::Type_1;
using RPM = detail::Type_4;
using Temperature = detail::Type_10;
using Precise = detail::Type_11;
using Mode = detail::Type_12;
using Samples = detail::Type_13;
using State = detail::Type_14;
using Config = detail::Type_15;
using ConfigureRequest = detail::Type_15;
using ResetRequest = detail::Type_0;
using EchoRequest = detail::Type_15;
using EchoResponse = detail::Type_15;
} // namespace motor::types

// Named types (descriptor v3.1), by their wire scope: adc::Config is types::adc::Config.
namespace types {
namespace fixture {
using Mode = ::device_api::detail::fixture::Mode;
using State = ::device_api::detail::fixture::State;
using MotorConfig = ::device_api::detail::fixture::MotorConfig;
} // namespace fixture
} // namespace types
} // namespace device_api

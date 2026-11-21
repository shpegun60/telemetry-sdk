// Generated from a validated Telemetry Descriptor v3. Do not edit.
// Descriptor SHA-256: f8ebd8f588cc54580ba53d3e45c2d1cabe747d05d81d8733b2b8b3648c52092e
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
using Type_5 = std::int16_t;
using Type_6 = std::uint32_t;
using Type_10 = float;
namespace adc {
struct Config {
    Type_10 gain;
    Type_4 rateHz;
    Type_1 enabled;
};
} // namespace adc
using Type_12 = adc::Config;
using Type_13 = std::array<Type_5, 2>;
namespace adc {
struct Status {
    Type_4 flags;
    Type_13 raw;
};
} // namespace adc
using Type_14 = adc::Status;
namespace metrology {
enum class Window : Type_2 {
    Rectangular = 0ULL,
    Hann = 1ULL,
    FlatTop = 2ULL,
};
} // namespace metrology
using Type_15 = metrology::Window;
namespace metrology {
struct Config {
    Type_6 cycles;
    Type_15 window;
};
} // namespace metrology
using Type_16 = metrology::Config;
namespace adc {
struct Reference {
    Type_10 volts;
};
} // namespace adc
using Type_17 = adc::Reference;
namespace adc {
struct Offset {
    Type_10 counts;
};
} // namespace adc
using Type_18 = adc::Offset;
} // namespace detail

namespace adc::types {
using Config = detail::Type_12;
using Status_value = detail::Type_14;
using ResetRequest = detail::Type_0;
using CalibrateRequest = detail::Type_17;
using CalibrateResponse = detail::Type_18;
} // namespace adc::types

namespace adc::channel1::types {
using Gain = detail::Type_10;
} // namespace adc::channel1::types

namespace adc::channel2::types {
using Gain = detail::Type_10;
} // namespace adc::channel2::types

namespace metrology::types {
using Config = detail::Type_16;
} // namespace metrology::types

// Named types (descriptor v3.1), by their wire scope: adc::Config is types::adc::Config.
namespace types {
namespace adc {
using Config = ::device_api::detail::adc::Config;
using Status = ::device_api::detail::adc::Status;
using Reference = ::device_api::detail::adc::Reference;
using Offset = ::device_api::detail::adc::Offset;
} // namespace adc
namespace metrology {
using Window = ::device_api::detail::metrology::Window;
using Config = ::device_api::detail::metrology::Config;
} // namespace metrology
} // namespace types
} // namespace device_api

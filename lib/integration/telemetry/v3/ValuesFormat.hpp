/*
 * @file ValuesFormat.hpp
 * @brief Live Values wire constants, shared by the device's writer and a host's reader.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The header-only module values_wire: ValuesFile writes this layout on the
 * device (resource_adapter), ValuesReader reads it on a host (values_reader).
 * The layout itself is doc/WireV3.md, "Values header and tokens".
 */
#ifndef TELEMETRY_INTEGRATION_V3_VALUES_FORMAT_HPP
#define TELEMETRY_INTEGRATION_V3_VALUES_FORMAT_HPP
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace resource::telemetry::v3 {

// Values carry a cached descriptor fingerprint, never a hash of live data.
inline constexpr std::uint16_t valuesHeaderBytes = 24;
// The header's first four bytes, then its version: the descriptor's own, 3.0.
inline constexpr std::array<std::byte, 4> valuesMagic{std::byte{'T'}, std::byte{'V'},
                                                      std::byte{'L'}, std::byte{'3'}};
inline constexpr std::uint16_t valuesMajor = 3;
inline constexpr std::uint16_t valuesMinor = 0;
enum class ValueStatus : std::uint8_t {
	Ok = 0,
	Unavailable = 1
};

} // namespace resource::telemetry::v3
#endif

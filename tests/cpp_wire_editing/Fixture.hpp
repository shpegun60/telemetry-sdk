/* Native application fixture for the optional standalone writer.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
#ifndef TELEMETRY_TESTS_CPP_WIRE_EDITING_FIXTURE_HPP
#define TELEMETRY_TESTS_CPP_WIRE_EDITING_FIXTURE_HPP
#pragma once
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <array>
#include <cstdint>

namespace wire_fixture {
enum class Mode : std::int16_t {
	Off = -7,
	On = 12
};

struct Leaf {
	float single;
	double wide;
	Mode mode;
	bool valid;
};

struct Config {
	std::uint32_t counter;
	std::array<Leaf, 3> leaves;
	bool tail;
};

struct Empty {};

struct Device {
	Config read() const noexcept
	{
		return {};
	}

	Empty empty() const noexcept
	{
		return {};
	}
};

inline Device device;
inline constexpr telemetry::Model model{
    telemetry::group("control", telemetry::field<&Device::read>("Config", device),
                     telemetry::field<&Device::empty>("Empty", device))};
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = telemetry::descriptor::packDescriptor<descriptor>();
} // namespace wire_fixture
#endif // TELEMETRY_TESTS_CPP_WIRE_EDITING_FIXTURE_HPP

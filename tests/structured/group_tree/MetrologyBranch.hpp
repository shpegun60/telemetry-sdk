/*
 * @file MetrologyBranch.hpp
 * @brief The second branch file of the group-tree fixture, with its own Config.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * A branch written without knowing the ADC branch: its Config is a different
 * type with the same short name, and its group holds a Field named Config
 * like the ADC's. Paths (metrology/Config) and type names
 * (metrology::Config) keep them apart.
 */

#ifndef TELEMETRY_TESTS_STRUCTURED_GROUP_TREE_METROLOGY_BRANCH_HPP
#define TELEMETRY_TESTS_STRUCTURED_GROUP_TREE_METROLOGY_BRANCH_HPP
#pragma once

#include <telemetry/Telemetry.hpp>

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

// API: config/setConfig.
struct Meter {
	Config current{10, Window::Hann};

	Config config() const noexcept
	{
		return current;
	}

	telemetry::WriteResult setConfig(const Config& value) noexcept
	{
		current = value;
		return telemetry::WriteResult::Applied;
	}
};

} // namespace metrology

namespace metrology::tlm {
namespace ts = telemetry;

inline Meter instance;

inline constexpr auto branch =
    ts::group("metrology", ts::field<&Meter::config, &Meter::setConfig>("Config", instance));

} // namespace metrology::tlm

#endif

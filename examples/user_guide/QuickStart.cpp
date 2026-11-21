/*
 * Small native Fields, Commands and Services application.
 *
 * Start with ordinary owner methods, put them in one group and reach them by
 * path through named handles. The static owner and Model satisfy borrowing
 * lifetimes; semantic validation stays in Device rather than the structural Model.
 *
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */

#include <telemetry/Telemetry.hpp>
#include <cassert>
#include <cmath>
#include <limits>

namespace ts = telemetry;

struct Config {
	float voltage;
	bool enabled;
};

struct Snapshot {
	float voltage;
	bool enabled;
};

// Public methods:
// - read(): Copy current configuration.
// - write(): Apply validated configuration.
// - reset(): Restore example defaults.
// - sample(): Copy measurement snapshot.
struct Device {
	Config state{230.0f, true};

	Config read() const noexcept
	{
		return state;
	}

	ts::WriteResult write(const Config& next) noexcept
	{
		if (!std::isfinite(next.voltage) || next.voltage < 0.0f || next.voltage > 300.0f)
			return ts::WriteResult::InvalidValue;
		state = next;
		return ts::WriteResult::Applied;
	}

	ts::CommandResult reset() noexcept
	{
		state = {230.0f, true};
		return ts::CommandResult::Executed;
	}

	Snapshot sample() const noexcept
	{
		return {state.voltage, state.enabled};
	}
};

inline Device device;
// One group holds its Field, Command and Service side by side: no tables, no IDs.
inline constexpr auto branch = ts::group(
    "device", ts::field<&Device::read, &Device::write>("Config", device),
    ts::command<&Device::reset>("Reset", device), ts::service<&Device::sample>("Sample", device));
inline constexpr ts::Model model{branch};
// Named handles: each path is checked here, once.
inline constexpr auto config = ts::endpoint<model, "device/Config">;
inline constexpr auto reset = ts::endpoint<model, "device/Reset">;
inline constexpr auto sample = ts::endpoint<model, "device/Sample">;

int main()
{
	assert(config.write(Config{240.0f, true}) == ts::WriteResult::Applied);
	const auto value = config.read();
	assert(value && value->voltage == 240.0f);
	assert(config.write(Config{std::numeric_limits<float>::quiet_NaN(), false}) ==
	       ts::WriteResult::InvalidValue);
	const auto unchanged = config.read();
	assert(unchanged && unchanged->voltage == 240.0f && unchanged->enabled);
	const auto voltage = ts::read<model, "device/Config.voltage">(); // One member, by path.
	assert(voltage && *voltage == 240.0f);
	const auto response = sample.call();
	assert(response.hasValue() && response.value().voltage == 240.0f);
	assert(reset.call() == ts::CommandResult::Executed);
}

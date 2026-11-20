/*
 * @file HandleSize.cpp
 * @brief ARM code of a named handle against the free path function and makeId access.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Every operation is written three ways: through a named handle
 * (endpoint<model, path>), through the free function (read<model, path>()) and
 * through a PackedId written with makeId (model.fields().read<id>()). run.py
 * compiles this file with -fno-ipa-icf, so identical functions stay separate,
 * and compares each function's instructions and relocations: a handle must
 * compile to exactly the free function's code, and a whole endpoint's handle
 * to exactly the makeId code. It also links handle_root and checks that the
 * loaded image holds no path text. These are offline object comparisons.
 */

#include "AdcBranch.hpp"
#include "MetrologyBranch.hpp"

#include <cstdint>

namespace handle_size {
namespace ts = telemetry;

inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};
inline constexpr auto gain1 = ts::endpoint<model, "adc/channel1/Gain">;
inline constexpr auto rate = ts::endpoint<model, "adc/Config.rateHz">;
inline constexpr auto reset = ts::endpoint<model, "adc/Reset">;
inline constexpr auto calibrate = ts::endpoint<model, "adc/Calibrate">;
inline constexpr auto gainId = ts::makeId<1, 0>();
inline constexpr auto configId = ts::makeId<0, 0>();
inline constexpr auto resetId = ts::makeId<0, 0>();
inline constexpr auto calibrateId = ts::makeId<0, 0>();

} // namespace handle_size

namespace hs = handle_size;
namespace ts = telemetry;

#define PROBE extern "C" __attribute__((used, noinline))

// Read a whole Field.
PROBE float read_handle() noexcept
{
	return hs::gain1.read().value_or(0.0f);
}

PROBE float read_path() noexcept
{
	return ts::read<hs::model, "adc/channel1/Gain">().value_or(0.0f);
}

PROBE float read_id() noexcept
{
	return hs::model.fields().read<hs::gainId>().value_or(0.0f);
}

// Read one member of a struct Field: the whole getter runs.
PROBE std::uint32_t member_handle() noexcept
{
	return hs::rate.read().value_or(0);
}

PROBE std::uint32_t member_path() noexcept
{
	return ts::read<hs::model, "adc/Config.rateHz">().value_or(0);
}

PROBE std::uint32_t member_id() noexcept
{
	const auto config = hs::model.fields().read<hs::configId>();
	return config ? config->rateHz : 0;
}

// Write a whole Field.
PROBE std::uint32_t write_handle(float value) noexcept
{
	return static_cast<std::uint32_t>(hs::gain1.write(value));
}

PROBE std::uint32_t write_path(float value) noexcept
{
	return static_cast<std::uint32_t>(ts::write<hs::model, "adc/channel1/Gain">(value));
}

PROBE std::uint32_t write_id(float value) noexcept
{
	return static_cast<std::uint32_t>(hs::model.fields().write<hs::gainId>(value));
}

// Call a Command.
PROBE std::uint32_t command_handle() noexcept
{
	return static_cast<std::uint32_t>(hs::reset.call());
}

PROBE std::uint32_t command_path() noexcept
{
	return static_cast<std::uint32_t>(ts::call<hs::model, "adc/Reset">());
}

PROBE std::uint32_t command_id() noexcept
{
	return static_cast<std::uint32_t>(hs::model.commands().call<hs::resetId>());
}

// Call a Service with its argument.
PROBE float service_handle(float volts) noexcept
{
	const auto offset = hs::calibrate.call(adc::Reference{volts});
	return offset.hasValue() ? offset.value().counts : -1.0f;
}

PROBE float service_path(float volts) noexcept
{
	const auto offset = ts::call<hs::model, "adc/Calibrate">(adc::Reference{volts});
	return offset.hasValue() ? offset.value().counts : -1.0f;
}

PROBE float service_id(float volts) noexcept
{
	const auto offset = hs::model.services().call<hs::calibrateId>(adc::Reference{volts});
	return offset.hasValue() ? offset.value().counts : -1.0f;
}

// The linked image's root: every probe is reachable, nothing else is kept.
extern "C" __attribute__((used)) std::uint32_t handle_root(std::uint32_t operation,
                                                           float value) noexcept
{
	switch (operation) {
		case 0:
			return static_cast<std::uint32_t>(read_handle() + read_path() + read_id());
		case 1:
			return member_handle() + member_path() + member_id();
		case 2:
			return write_handle(value) + write_path(value) + write_id(value);
		case 3:
			return command_handle() + command_path() + command_id();
		default:
			return static_cast<std::uint32_t>(service_handle(value) + service_path(value) +
			                                  service_id(value));
	}
}

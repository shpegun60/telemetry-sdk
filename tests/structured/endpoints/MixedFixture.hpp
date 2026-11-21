/*
 * @file MixedFixture.hpp
 * @brief One mixed group of all three endpoint families and a type they share.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#ifndef TELEMETRY_TESTS_STRUCTURED_ENDPOINTS_MIXEDFIXTURE_HPP
#define TELEMETRY_TESTS_STRUCTURED_ENDPOINTS_MIXEDFIXTURE_HPP
#pragma once

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>

namespace fixture {
namespace ts = telemetry;
enum class Mode : std::uint8_t {
	Off,
	Run,
	Fault
};
enum class Position : std::uint8_t {
	Enabled,
	Rpm,
	Temperature,
	Precise,
	Mode,
	Samples,
	State,
	Config
};

// Shared exact DTO registered once across Field, Command and Service roots.
struct MotorConfig {
	float target;
	std::uint16_t rpm;
	bool enabled;
};

// Small composite Field output with independent floating and boolean leaves.
struct State {
	float volts;
	bool active;
};

// Counted mutable owner; configurable statuses expose preflight/callback ordering.
// API: enabled/rpm/temperature/precise/mode/samples/state/config read native values;
// setConfig writes; configure/reset execute Commands; echo returns a Service payload.
struct Device {
	MotorConfig current{230.0f, 1500, true};
	int reads = 0;
	int writes = 0;
	int commands = 0;
	int services = 0;
	telemetry::WriteResult writeStatus = telemetry::WriteResult::Applied;
	telemetry::CommandResult commandStatus = telemetry::CommandResult::Executed;

	bool enabled() noexcept
	{
		++reads;
		return current.enabled;
	}

	std::uint16_t rpm() noexcept
	{
		++reads;
		return current.rpm;
	}

	float temperature() noexcept
	{
		++reads;
		return 21.5f;
	}

	double precise() noexcept
	{
		++reads;
		return 0.125;
	}

	Mode mode() noexcept
	{
		++reads;
		return Mode::Run;
	}

	std::array<std::uint16_t, 3> samples() noexcept
	{
		++reads;
		return {1, 256, 65535};
	}

	State state() noexcept
	{
		++reads;
		return {current.target, current.enabled};
	}

	MotorConfig config() noexcept
	{
		++reads;
		return current;
	}

	telemetry::WriteResult setConfig(const MotorConfig& value) noexcept
	{
		++writes;
		current = value;
		return writeStatus;
	}

	telemetry::CommandResult configure(const MotorConfig& value) noexcept
	{
		++commands;
		current = value;
		return commandStatus;
	}

	telemetry::CommandResult reset() noexcept
	{
		++commands;
		return commandStatus;
	}

	MotorConfig echo(const MotorConfig& value) noexcept
	{
		++services;
		return value;
	}
};

inline Device device;
// The endpoint definitions, shared by the device's tree and the catalog twin.
inline constexpr auto enabledField = ts::field<&Device::enabled>("Enabled", device);
inline constexpr auto rpmField = ts::field<&Device::rpm>("RPM", device);
inline constexpr auto temperatureField = ts::field<&Device::temperature>("Temperature", device);
inline constexpr auto preciseField = ts::field<&Device::precise>("Precise", device);
inline constexpr auto modeField = ts::field<&Device::mode>("Mode", device);
inline constexpr auto samplesField = ts::field<&Device::samples>("Samples", device);
inline constexpr auto stateField = ts::field<&Device::state>("State", device);
inline constexpr auto configField =
    ts::field<&Device::config, &Device::setConfig>("Config", device);
inline constexpr auto configureCommand = ts::command<&Device::configure>("Configure", device);
inline constexpr auto resetCommand = ts::command<&Device::reset>("Reset", device);
inline constexpr auto echoService = ts::service<&Device::echo>("Echo", device);

// The device: one group, motor, holding its eight Fields, two Commands and a
// Service. Every family's IDs are group 0 and the declaration position.
inline constexpr ts::Model model{
    ts::group("motor", enabledField, rpmField, temperatureField, preciseField, modeField,
              samplesField, stateField, configField, configureCommand, resetCommand, echoService)};
inline constexpr auto config = ts::endpoint<model, "motor/Config">;
inline constexpr auto configure = ts::endpoint<model, "motor/Configure">;
inline constexpr auto reset = ts::endpoint<model, "motor/Reset">;
inline constexpr auto echo = ts::endpoint<model, "motor/Echo">;

// The same endpoints in per-family local tables, and the catalog twin over
// them: the producer of the frozen descriptor v3.0 golden mixed.hex
// (telemetry/testing/V30Model.hpp). It has the tree's IDs, one catalog per family.
inline constexpr ts::FieldTable mixedFields{enabledField, rpmField,   temperatureField,
                                            preciseField, modeField,  samplesField,
                                            stateField,   configField};
inline constexpr ts::CommandTable localCommands{configureCommand, resetCommand};
inline constexpr ts::ServiceTable localServices{echoService};
inline constexpr ts::testing::FieldCatalogTable v30Fields{ts::testing::group("motor", mixedFields)};
inline constexpr ts::testing::CommandCatalogTable v30Commands{
    ts::testing::group("motor", localCommands)};
inline constexpr ts::testing::ServiceCatalogTable v30Services{
    ts::testing::group("motor", localServices)};
inline constexpr ts::testing::V30Model v30Model{v30Fields, v30Commands, v30Services};
} // namespace fixture

#endif // TELEMETRY_TESTS_STRUCTURED_ENDPOINTS_MIXEDFIXTURE_HPP

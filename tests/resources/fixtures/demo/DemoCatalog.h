/*
 * Native test types and stable bindings for Resource facade checks.
 *
 * Make application validation visible beside ordinary C++ getters and setters.
 * Inline owners outlive the tables that borrow them; reflected member shapes
 * carry no presentation units or application limits.
 *
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */

#ifndef DEMO_CATALOG_H
#define DEMO_CATALOG_H
#pragma once

#include <telemetry/Telemetry.hpp>
#include <cmath>
#include <cstdint>
#include <limits>

namespace demo {

// A nested aggregate is one Field value. Reflection describes its members;
// the codec sends their canonical bytes without native padding.
struct NestedValue {
	float f32;
	double f64;
	std::int32_t s32;
};

struct StructuredValue {
	NestedValue nested;
	float f32;
	double f64;
	std::int32_t s32;
};

// Public methods:
// - temperature(): Read simulated temperature.
// - enabled(): Read simulated availability.
// - advance(): Advance simulated state.
class Sensor {
public:
	double temperature() const noexcept;
	bool enabled() const noexcept;
	void advance() noexcept;

private:
	double temperature_ = 24.0;
	bool enabled_ = true;
};

using telemetry::field;
using telemetry::WriteResult;
using telemetry::CommandResult;

enum class Mode : std::uint16_t {
	Off,
	Auto,
	Manual
};
enum class MeterField : std::size_t {
	Voltage,
	Current,
	Power,
	Counter,
	VoltageLimit,
	Mode,
	Structured,
	Count
};
enum class MeterCommand : std::size_t {
	Reset,
	Configure,
	Count
};

// This is one ordinary aggregate request, not a second parameter type list.
struct ConfigureRequest {
	float voltageLimit;
	Mode mode;
};

// The old descriptor limits are now checked where their decisions belong.
inline constexpr float minimumVoltageLimit = 1.0f;
inline constexpr float maximumVoltageLimit = 1000.0f;

constexpr bool validMode(Mode mode) noexcept
{
	switch (mode) {
		case Mode::Off:
		case Mode::Auto:
		case Mode::Manual:
			return true;
	}
	return false;
}

inline bool validVoltageLimit(float value) noexcept
{
	return std::isfinite(value) && value >= minimumVoltageLimit && value <= maximumVoltageLimit;
}

// Public methods:
// - readVoltage(): Read simulated voltage.
// - readCurrent(): Read simulated current.
// - readPower(): Calculate simulated power.
// - readCounter(): Read tick counter.
// - readThreshold(): Read configured limit.
// - readMode(): Read configured mode.
// - setMode(): Validate mode update.
// - setThreshold(): Validate limit update.
// - reset(): Clear tick counter.
// - configure(): Apply validated request.
// - readStructured(): Read nested sample.
struct Meter {
	float voltage = 230.0f;
	float current = 2.0f;
	std::uint32_t counter = 0;
	float threshold = 250.0f;
	Mode mode = Mode::Auto;

	float readVoltage() const noexcept
	{
		return voltage;
	}

	float readCurrent() const noexcept
	{
		return current;
	}

	float readPower() const noexcept
	{
		return voltage * current / 1000.0f;
	}

	std::uint32_t readCounter() const noexcept
	{
		return counter;
	}

	float readThreshold() const noexcept
	{
		return threshold;
	}

	Mode readMode() const noexcept
	{
		return mode;
	}

	WriteResult setMode(Mode next) noexcept
	{
		if (!validMode(next))
			return WriteResult::InvalidValue;
		mode = next;
		return WriteResult::Applied;
	}

	WriteResult setThreshold(float next) noexcept
	{
		if (!validVoltageLimit(next))
			return WriteResult::InvalidValue;
		threshold = next;
		return WriteResult::Applied;
	}

	CommandResult reset() noexcept
	{
		counter = 0;
		return CommandResult::Executed;
	}

	CommandResult configure(const ConfigureRequest& request) noexcept
	{
		// Refuse the whole request before changing either member. The GUI and
		// encoded callers get the same application validation and result.
		if (!validVoltageLimit(request.voltageLimit) || !validMode(request.mode))
			return CommandResult::InvalidValue;
		threshold = request.voltageLimit;
		mode = request.mode;
		return CommandResult::Executed;
	}

	StructuredValue readStructured() const noexcept
	{
		return {{1.25f, 2.5, -3}, 123.456f, 789.0, -11};
	}
};

inline Meter meter;
inline Sensor sensor;

// Every definition uses only a name and its native binding.
inline constexpr telemetry::FieldTable meterFields{
    field<&Meter::readVoltage>("Ua", meter),
    field<&Meter::readCurrent>("Ia", meter),
    field<&Meter::readPower>("P", meter),
    field<&Meter::readCounter>("WinCnt", meter),
    field<&Meter::readThreshold, &Meter::setThreshold>("VoltageLimit", meter),
    field<&Meter::readMode, &Meter::setMode>("Mode", meter),
    field<&Meter::readStructured>("Structured", meter),
};
static_assert(meterFields.size() == static_cast<std::size_t>(MeterField::Count));
static_assert(telemetry::wireSize<StructuredValue> == 32);

// Native integer return types preserve every bit, including the U64/S64 ends.
template<class T>
T maximumValue() noexcept
{
	return std::numeric_limits<T>::max();
}

template<class T>
T minimumValue() noexcept
{
	return std::numeric_limits<T>::lowest();
}

inline constexpr telemetry::FieldTable integerFields{
    field<&maximumValue<std::uint8_t>>("U8"),   field<&maximumValue<std::uint16_t>>("U16"),
    field<&maximumValue<std::uint32_t>>("U32"), field<&maximumValue<std::uint64_t>>("U64"),
    field<&minimumValue<std::int8_t>>("S8"),    field<&minimumValue<std::int16_t>>("S16"),
    field<&minimumValue<std::int32_t>>("S32"),  field<&minimumValue<std::int64_t>>("S64"),
};

inline constexpr telemetry::CommandTable meterCommands{
    telemetry::command<&Meter::reset>("Reset counter", meter),
    telemetry::command<&Meter::configure>("Configure meter", meter),
};
static_assert(meterCommands.size() == static_cast<std::size_t>(MeterCommand::Count));
inline constexpr telemetry::CommandCatalogTable commands{
    telemetry::group("meter", meterCommands),
};
inline constexpr auto commandIndex = commands.index();

inline constexpr telemetry::FieldTable sensorFields{
    field<&Sensor::temperature>("Temperature", sensor),
    field<&Sensor::enabled>("Enabled", sensor),
};
inline constexpr telemetry::FieldCatalogTable fields{
    telemetry::group("meter", meterFields),
    telemetry::group("sensor", sensorFields),
    telemetry::group("integers", integerFields),
};
inline constexpr auto fieldIndex = fields.index();
inline constexpr telemetry::ServiceCatalogTable<> services{};
inline constexpr telemetry::Model model{fields, commands, services};

void advance() noexcept;

} // namespace demo
#endif

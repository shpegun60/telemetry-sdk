/*
 * @file ArmCodegen.cpp
 * @brief Direct/local/handle/path native codegen and actual runtime entry layouts.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/model/Model.hpp>
#include "MixedFixture.hpp"
namespace ts = telemetry;

namespace probe {
struct Request {
	std::uint32_t value;
};

// Counted native owner supplies direct, local-table, handle and path ARM roots.
// API: get(), set(), call().
struct Device {
	std::uint32_t value = 9;

	std::uint32_t get() const noexcept
	{
		return value;
	}

	telemetry::WriteResult set(std::uint32_t next) noexcept
	{
		value = next;
		return telemetry::WriteResult::Applied;
	}

	telemetry::CommandResult call(const Request& request) noexcept
	{
		value = request.value;
		return telemetry::CommandResult::Executed;
	}
};

Device device;
inline constexpr ts::FieldTable fields{ts::field<&Device::get, &Device::set>("value", device)};
inline constexpr ts::CommandTable commands{ts::command<&Device::call>("set", device)};
inline constexpr ts::Model model{ts::group("test",
                                           ts::field<&Device::get, &Device::set>("value", device),
                                           ts::command<&Device::call>("set", device))};
inline constexpr auto value = ts::endpoint<model, "test/value">;
inline constexpr auto set = ts::endpoint<model, "test/set">;
} // namespace probe

#define PROBE extern "C" __attribute__((noinline))

PROBE std::optional<std::uint32_t> read_direct() noexcept
{
	return probe::device.get();
}

PROBE std::optional<std::uint32_t> read_local() noexcept
{
	return probe::fields.read<0>();
}

PROBE std::optional<std::uint32_t> read_handle() noexcept
{
	return probe::value.read();
}

PROBE std::optional<std::uint32_t> read_path() noexcept
{
	return ts::read<probe::model, "test/value">();
}

PROBE telemetry::WriteResult write_direct(std::uint32_t value) noexcept
{
	return probe::device.set(value);
}

PROBE telemetry::WriteResult write_local(std::uint32_t value) noexcept
{
	return probe::fields.write<0>(value);
}

PROBE telemetry::WriteResult write_handle(std::uint32_t value) noexcept
{
	return probe::value.write(value);
}

PROBE telemetry::WriteResult write_path(std::uint32_t value) noexcept
{
	return ts::write<probe::model, "test/value">(value);
}

PROBE telemetry::CommandResult command_direct(const probe::Request& value) noexcept
{
	return probe::device.call(value);
}

PROBE telemetry::CommandResult command_local(const probe::Request& value) noexcept
{
	return probe::commands.call<0>(value);
}

PROBE telemetry::CommandResult command_handle(const probe::Request& value) noexcept
{
	return probe::set.call(value);
}

PROBE telemetry::CommandResult command_path(const probe::Request& value) noexcept
{
	return ts::call<probe::model, "test/set">(value);
}

// Return-type and data-shape differences must not reintroduce runtime type
// switches when the position and binding are known to the compiler. The path
// route of these reads is in ArmCodegenPath.cpp: with a handle and a path
// reading the tree's table in one unit, GCC 13 at -Os keeps the shared read<I>
// out of line instead of inlining it at both call sites.
#define MIXED_READ(label, index, path, method, Value)                                              \
	PROBE std::optional<Value> label##_direct() noexcept                                           \
	{                                                                                              \
		return fixture::device.method();                                                           \
	}                                                                                              \
	PROBE std::optional<Value> label##_local() noexcept                                            \
	{                                                                                              \
		return fixture::mixedFields.read<index>();                                                 \
	}                                                                                              \
	PROBE std::optional<Value> label##_handle() noexcept                                           \
	{                                                                                              \
		return ts::endpoint<fixture::model, path>.read();                                          \
	}
MIXED_READ(boolean, 0, "motor/Enabled", enabled, bool)
MIXED_READ(u16, 1, "motor/RPM", rpm, std::uint16_t)
MIXED_READ(f32, 2, "motor/Temperature", temperature, float)
MIXED_READ(f64, 3, "motor/Precise", precise, double)
MIXED_READ(enumeration, 4, "motor/Mode", mode, fixture::Mode)
using Samples = std::array<std::uint16_t, 3>;
MIXED_READ(array, 5, "motor/Samples", samples, Samples)
MIXED_READ(state, 6, "motor/State", state, fixture::State)
MIXED_READ(config, 7, "motor/Config", config, fixture::MotorConfig)

#define ENTRY_LAYOUT(T, Context) sizeof(ts::T), alignof(ts::T), offsetof(ts::T, Context)
extern "C" __attribute__((used, section(".rodata.endpoint_layout")))
const std::uint32_t endpoint_layout[] = {
    ENTRY_LAYOUT(FieldEntry, readContext), offsetof(ts::FieldEntry, read),
    offsetof(ts::FieldEntry, write),       ENTRY_LAYOUT(CommandEntry, context),
    offsetof(ts::CommandEntry, invoke),    ENTRY_LAYOUT(ServiceEntry, context),
    offsetof(ts::ServiceEntry, invoke)};

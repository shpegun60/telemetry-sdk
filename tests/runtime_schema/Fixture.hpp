/*
 * @file Fixture.hpp
 * @brief Native producer model for runtime schema and dynamic value checks.
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_TEST_RUNTIME_SCHEMA_FIXTURE_HPP
#define TELEMETRY_TEST_RUNTIME_SCHEMA_FIXTURE_HPP
#pragma once

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/encoded/Dispatcher.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>

namespace runtime_fixture {

enum class Mode : std::int16_t {
	Negative = -7,
	Positive = 12
};
enum class Wide : std::uint64_t {
	Low = 0,
	High = UINT64_MAX
};

struct Empty {};

struct Inner {
	std::uint16_t code;
	std::array<std::int8_t, 3> bins;
	Mode mode;
	bool ready;
};

struct Packet {
	bool flag;
	std::uint8_t u8;
	std::int8_t i8;
	std::uint16_t u16;
	std::int16_t i16;
	std::uint32_t u32;
	std::int32_t i32;
	std::uint64_t u64;
	std::int64_t i64;
	float f32;
	double f64;
	Inner nested;
	std::array<Inner, 2> rows;
	std::array<std::array<std::uint32_t, 2>, 2> matrix;
	std::array<Empty, 2> empty;
	std::array<std::uint16_t, 0> zero;
	Wide wide;
};

inline constexpr Packet initial{
    true,
    UINT8_MAX,
    INT8_MIN,
    UINT16_MAX,
    INT16_MIN,
    UINT32_MAX,
    INT32_MIN,
    UINT64_MAX,
    INT64_MIN,
    -0.0f,
    std::numeric_limits<double>::infinity(),
    {0x1234, {-128, 0, 127}, Mode::Negative, true},
    {{{1, {1, 2, 3}, Mode::Positive, false}, {65535, {-1, -2, -3}, static_cast<Mode>(-2), true}}},
    {{{0x01020304, 0xaabbccdd}, {42, UINT32_MAX}}},
    {},
    {},
    Wide::High};

} // namespace runtime_fixture

namespace telemetry::reflection {
template<>
struct EnumReflection<runtime_fixture::Wide> {
	inline static constexpr auto entries =
	    enumEntries(enumEntry(runtime_fixture::Wide::Low, "Low"),
	                enumEntry(runtime_fixture::Wide::High, "High"));
};
} // namespace telemetry::reflection

namespace runtime_fixture {

struct Device {
	Packet packet = initial;
	unsigned reads = 0;

	Packet read() noexcept
	{
		++reads;
		return packet;
	}

	telemetry::WriteResult write(const Packet& value) noexcept
	{
		packet = value;
		return telemetry::WriteResult::Applied;
	}

	Empty readEmpty() const noexcept
	{
		return {};
	}

	std::array<std::uint16_t, 0> readZero() const noexcept
	{
		return {};
	}

	std::array<Empty, 65536> readHugeZero() const noexcept
	{
		return {};
	}

	telemetry::CommandResult apply(const Packet& value) noexcept
	{
		packet = value;
		return telemetry::CommandResult::Executed;
	}

	telemetry::ServiceResult<Packet> inspect() const noexcept
	{
		return telemetry::ServiceResult<Packet>::success(packet);
	}
};

inline Device device;
inline constexpr telemetry::FieldTable localFields{
    telemetry::field<&Device::read, &Device::write>("Packet", device),
    telemetry::field<&Device::readEmpty>("Empty", device),
    telemetry::field<&Device::readZero>("Zero", device),
    telemetry::field<&Device::readHugeZero>("HugeZero", device)};
inline constexpr telemetry::testing::FieldCatalogTable fields{
    telemetry::testing::group("dynamic", localFields)};
inline constexpr telemetry::CommandTable localCommands{
    telemetry::command<&Device::apply>("Apply", device)};
inline constexpr telemetry::ServiceTable localServices{
    telemetry::service<&Device::inspect>("Inspect", device)};
inline constexpr telemetry::testing::CommandCatalogTable commands{
    telemetry::testing::group("dynamic", localCommands)};
inline constexpr telemetry::testing::ServiceCatalogTable services{
    telemetry::testing::group("dynamic", localServices)};
// The native v3.0 descriptor the mutation corpus rewrites at fixed offsets: a
// catalog model of the testing header.
inline constexpr telemetry::testing::V30Model model{fields, commands, services};
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = telemetry::descriptor::packDescriptor<descriptor>();

} // namespace runtime_fixture
#endif // TELEMETRY_TEST_RUNTIME_SCHEMA_FIXTURE_HPP

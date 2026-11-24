/* Typed traversal fixtures. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */

#ifndef TELEMETRY_TESTS_STRUCTURED_TRAVERSAL_FIXTURE_HPP
#define TELEMETRY_TESTS_STRUCTURED_TRAVERSAL_FIXTURE_HPP
#pragma once

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>

namespace fixture {
namespace ts = telemetry;
using W = telemetry::WriteResult;
using C = telemetry::CommandResult;
enum class Mode : std::int16_t {
	Off = -1,
	On = 2
};
enum class Position : std::uint8_t {
	Boolean,
	U16,
	Float,
	S64,
	U64,
	Mode,
	Array,
	Config,
	Big,
	Slot
};

// Exact composite DTO used by Fields, Commands and Services during typed traversal.
struct Config {
	std::uint32_t code;
	bool enabled;
};

// 4 KiB payload exposes owning native copies versus caller-scratch encoded paths.
struct Big {
	std::array<std::uint32_t, 1024> words;
};

// Mutable endpoint owner with separate read/write/Command/Service invocation counters.
// API: readBool/readU16/readFloat/readS64/readU64/readMode/readArray/readConfig/readBig
// are getters; set* write Fields; configure/reset are Commands; echo/ping/large Services.
struct Device {
	unsigned reads = 0, writes = 0, commands = 0, services = 0;
	bool boolean = true;
	std::uint16_t integer = 65535;
	double real = 12.75;
	std::int64_t signedValue = INT64_MIN;
	std::uint64_t unsignedValue = UINT64_MAX;
	Mode modeValue = Mode::On;
	Config config{42, true};

	bool readBool() noexcept
	{
		++reads;
		return boolean;
	}

	std::uint16_t readU16() noexcept
	{
		++reads;
		return integer;
	}

	double readFloat() noexcept
	{
		++reads;
		return real;
	}

	std::int64_t readS64() noexcept
	{
		++reads;
		return signedValue;
	}

	std::uint64_t readU64() noexcept
	{
		++reads;
		return unsignedValue;
	}

	Mode readMode() noexcept
	{
		++reads;
		return modeValue;
	}

	std::array<std::uint16_t, 2> readArray() noexcept
	{
		++reads;
		return {1, 65535};
	}

	Config readConfig() noexcept
	{
		++reads;
		return config;
	}

	Big readBig() noexcept
	{
		++reads;
		return Big{{1, 2, 3}};
	}

	W setBool(bool value) noexcept
	{
		++writes;
		boolean = value;
		return W::Applied;
	}

	W setU16(std::uint16_t value) noexcept
	{
		++writes;
		integer = value;
		return W::Applied;
	}

	W setFloat(double value) noexcept
	{
		++writes;
		real = value;
		return W::Applied;
	}

	W setS64(std::int64_t value) noexcept
	{
		++writes;
		signedValue = value;
		return W::Applied;
	}

	W setU64(std::uint64_t value) noexcept
	{
		++writes;
		unsignedValue = value;
		return W::Applied;
	}

	W setMode(Mode value) noexcept
	{
		++writes;
		modeValue = value;
		return W::Applied;
	}

	W setConfig(const Config& value) noexcept
	{
		++writes;
		config = value;
		return W::Applied;
	}

	W setBig(const Big&) noexcept
	{
		++writes;
		return W::Applied;
	}

	C configure(const Config& value) noexcept
	{
		++commands;
		config = value;
		return C::Executed;
	}

	C reset() noexcept
	{
		++commands;
		return C::Executed;
	}

	Config echo(const Config& value) noexcept
	{
		++services;
		return value;
	}

	void ping() noexcept
	{
		++services;
	}

	Big large() noexcept
	{
		++services;
		return Big{{1, 2, 3}};
	}
};

inline Device device;
inline telemetry::OwnerSlot<Device> slot;
inline constexpr ts::FieldTable localFields{
    ts::field<&Device::readBool, &Device::setBool>("Bool", device),
    ts::field<&Device::readU16, &Device::setU16>("U16", device),
    ts::field<&Device::readFloat, &Device::setFloat>("Float", device),
    ts::field<&Device::readS64, &Device::setS64>("S64", device),
    ts::field<&Device::readU64, &Device::setU64>("U64", device),
    ts::field<&Device::readMode, &Device::setMode>("Mode", device),
    ts::field<&Device::readArray>("Array", device),
    ts::field<&Device::readConfig, &Device::setConfig>("Config", device),
    ts::field<&Device::readBig, &Device::setBig>("Big", device),
    ts::field<&Device::readConfig, &Device::setConfig>("Slot", slot)};
inline constexpr ts::CommandTable localCommands{
    ts::command<&Device::configure>("Configure", device),
    ts::command<&Device::reset>("Reset", device), ts::command<&Device::configure>("Slot", slot)};
inline constexpr ts::ServiceTable localServices{
    ts::service<&Device::echo>("Echo", device), ts::service<&Device::ping>("Ping", device),
    ts::service<&Device::large>("Big", device), ts::service<&Device::echo>("Slot", slot)};
inline constexpr ts::FieldTable<> noFields{};
inline constexpr ts::CommandTable<> noCommands{};
inline constexpr ts::ServiceTable<> noServices{};
inline constexpr ts::testing::FieldCatalogTable<> noFieldCatalogs{};
inline constexpr ts::testing::CommandCatalogTable<> noCommandCatalogs{};
inline constexpr ts::testing::ServiceCatalogTable<> noServiceCatalogs{};

// The tree: first, an empty group and repeat, each with ten Fields, three
// Commands and four Services (the same endpoints as the local tables; one
// group's names are unique across its families). Typed traversal and As
// access go through the tree's catalog tables, one row per group.
inline constexpr auto traversalGroup(const char* name) noexcept
{
	return ts::group(
	    name, ts::field<&Device::readBool, &Device::setBool>("Bool", device),
	    ts::field<&Device::readU16, &Device::setU16>("U16", device),
	    ts::field<&Device::readFloat, &Device::setFloat>("Float", device),
	    ts::field<&Device::readS64, &Device::setS64>("S64", device),
	    ts::field<&Device::readU64, &Device::setU64>("U64", device),
	    ts::field<&Device::readMode, &Device::setMode>("Mode", device),
	    ts::field<&Device::readArray>("Array", device),
	    ts::field<&Device::readConfig, &Device::setConfig>("Config", device),
	    ts::field<&Device::readBig, &Device::setBig>("Big", device),
	    ts::field<&Device::readConfig, &Device::setConfig>("Slot", slot),
	    ts::command<&Device::configure>("Configure", device),
	    ts::command<&Device::reset>("Reset", device),
	    ts::command<&Device::configure>("SlotConfigure", slot),
	    ts::service<&Device::echo>("Echo", device), ts::service<&Device::ping>("Ping", device),
	    ts::service<&Device::large>("Large", device), ts::service<&Device::echo>("SlotEcho", slot));
}

inline constexpr ts::Model model{traversalGroup("first"), ts::group("empty"),
                                 traversalGroup("repeat")};
inline constexpr auto& fields = model.fields();
inline constexpr auto& commands = model.commands();
inline constexpr auto& services = model.services();
} // namespace fixture

#endif // TELEMETRY_TESTS_STRUCTURED_TRAVERSAL_FIXTURE_HPP

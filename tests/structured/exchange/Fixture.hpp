/* Optional Bind/Exchange native model, counted owners and canonical packet fixtures. MIT. */

#ifndef TELEMETRY_TESTS_STRUCTURED_EXCHANGE_FIXTURE_HPP
#define TELEMETRY_TESTS_STRUCTURED_EXCHANGE_FIXTURE_HPP
#pragma once

#include <structured_protocol/Bind.hpp>
#include <structured_protocol/Exchange.hpp>
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <resource/FileSystem.hpp>
#include <algorithm>
#include <array>
#include <cassert>

namespace fixture {
namespace ts = telemetry;
namespace rs = example::structured_protocol;
using D = rs::PacketStatus;
using W = telemetry::WriteResult;
using C = telemetry::CommandResult;
using S = ts::ServiceStatus;
using Op = rs::Operation;
using resource::Input;
using resource::Output;

// Small control payload shared by all endpoint categories in packet checks.
struct Config {
	std::uint32_t value;
	bool enabled;
};

// 4 KiB DTO exercises capacity refusal before side effects and scratch use.
struct Big {
	std::array<std::uint32_t, 1024> words;
};

// Counted fake owner for successful, refused, unavailable and large control routes.
// API: read/readBig get Fields; write/writeBig set them; configure/reset/commandBig
// execute Commands; echo/ping/large return Service status with typed payloads.
struct Device {
	Config current{42, true};
	unsigned reads = 0, writes = 0, commands = 0, services = 0;
	W writeStatus = W::Applied;
	C commandStatus = C::Executed;
	S serviceStatus = S::Ok;

	Config read() noexcept
	{
		++reads;
		return current;
	}

	W write(const Config& value) noexcept
	{
		++writes;
		current = value;
		return writeStatus;
	}

	C configure(const Config& value) noexcept
	{
		++commands;
		current = value;
		return commandStatus;
	}

	C reset() noexcept
	{
		++commands;
		return commandStatus;
	}

	ts::ServiceResult<Config> echo(const Config& value) noexcept
	{
		++services;
		if (serviceStatus != S::Ok)
			return ts::ServiceResult<Config>::failure(serviceStatus);
		return ts::ServiceResult<Config>::success(value);
	}

	ts::ServiceResult<void> ping() noexcept
	{
		++services;
		return serviceStatus == S::Ok ? ts::ServiceResult<void>::success()
		                              : ts::ServiceResult<void>::failure(serviceStatus);
	}

	ts::ServiceResult<Big> large(const Config& value) noexcept
	{
		++services;
		current = value; // A deliberate side effect must not precede capacity checks.
		if (serviceStatus != S::Ok)
			return ts::ServiceResult<Big>::failure(serviceStatus);
		return ts::ServiceResult<Big>::successFrom([&]() noexcept {
			return Big{{value.value}};
		});
	}

	Big readBig() noexcept
	{
		++reads;
		return Big{{current.value}};
	}

	W writeBig(const Big& value) noexcept
	{
		++writes;
		current.value = value.words.back();
		return writeStatus;
	}

	C commandBig(const Big& value) noexcept
	{
		++commands;
		current.value = value.words.back();
		return commandStatus;
	}
};

inline Device device;
inline telemetry::OwnerSlot<Device> slot;
inline constexpr auto configField = ts::field<&Device::read, &Device::write>("Config", device);
inline constexpr auto readOnlyField = ts::field<&Device::read>("ReadOnly", device);
inline constexpr auto optionalField = ts::field<&Device::read, &Device::write>("Optional", slot);
inline constexpr auto bigField = ts::field<&Device::readBig, &Device::writeBig>("Big", device);
inline constexpr auto configureCommand = ts::command<&Device::configure>("Configure", device);
inline constexpr auto resetCommand = ts::command<&Device::reset>("Reset", device);
inline constexpr auto optionalCommand = ts::command<&Device::configure>("OptionalConfigure", slot);
inline constexpr auto bigCommand = ts::command<&Device::commandBig>("BigCommand", device);
inline constexpr auto echoService = ts::service<&Device::echo>("Echo", device);
inline constexpr auto pingService = ts::service<&Device::ping>("Ping", device);
inline constexpr auto optionalService = ts::service<&Device::echo>("OptionalEcho", slot);
inline constexpr auto largeService = ts::service<&Device::large>("Large", device);
// One group, device, of four Fields, four Commands and four Services; a
// group's names are unique across its families.
inline constexpr ts::Model model{ts::group(
    "device", configField, readOnlyField, optionalField, bigField, configureCommand, resetCommand,
    optionalCommand, bigCommand, echoService, pingService, optionalService, largeService)};
inline constexpr auto view = model.view();
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto packed = ::telemetry::descriptor::packDescriptor<descriptor>();
inline constexpr resource::telemetry::v3::DescriptorFile descriptorFile{packed};

// Test helpers deliberately do not use the protocol implementation's helpers.
template<class U>
void put(Output bytes, std::size_t offset, U value) noexcept
{
	for (std::size_t i = 0; i < sizeof(U); ++i)
		bytes[offset + i] = std::byte(value >> (i * 8));
}

inline std::uint32_t get32(Input bytes, std::size_t offset) noexcept
{
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
	return value;
}

inline std::array<std::byte, 16>
handshake(std::uint64_t fingerprint = descriptor.fingerprint()) noexcept
{
	std::array<std::byte, 16> result{std::byte{'T'}, std::byte{'S'}, std::byte{'B'}, std::byte{'N'},
	                                 std::byte{3}};
	put(result, 8, fingerprint);
	return result;
}

inline void header(Output bytes, Op operation, std::uint32_t endpoint, std::uint32_t payload,
                   std::uint32_t correlation = 0x10203040) noexcept
{
	// Retained firmware embeds assert locations. Keep diagnostic line constants
	// stable across comment/format edits; no condition or runtime gate changes.
#line 107
	assert(bytes.size() >= 24);
	std::fill_n(bytes.begin(), 24, std::byte{0});
	bytes[0] = std::byte{'T'};
	bytes[1] = std::byte{'S'};
	bytes[2] = std::byte{'R'};
	bytes[3] = std::byte{'Q'};
	bytes[4] = std::byte{3};
	put(bytes, 8, correlation);
	put(bytes, 12, endpoint);
	put(bytes, 16, payload);
	bytes[20] = static_cast<std::byte>(operation);
}

template<class T>
std::array<std::byte, 24 + ts::wireSize<T>> packet(Op operation, std::uint32_t id,
                                                   const T& value) noexcept
{
	std::array<std::byte, 24 + ts::wireSize<T>> result{};
	header(result, operation, id, ts::wireSize<T>);
	const auto status = ts::encode(value, Output{result}.subspan(24));
#line 120
	assert(status == ts::CodecStatus::Ok);
	(void)status;
	return result;
}

inline std::array<std::byte, 24> emptyPacket(Op operation, std::uint32_t id) noexcept
{
	std::array<std::byte, 24> result{};
	header(result, operation, id, 0);
	return result;
}

inline void ready(rs::Binding& peer) noexcept
{
	std::array<std::byte, 8> response{};
	const auto result =
	    rs::Bind::process(peer, view, descriptor.fingerprint(), handshake(), response);
#line 134
	assert(result.dispatch == D::Ok && result.written == 8 && peer.ready());
}

inline unsigned callbackCount() noexcept
{
	return device.writes + device.commands + device.services;
}
} // namespace fixture

#endif // TELEMETRY_TESTS_STRUCTURED_EXCHANGE_FIXTURE_HPP

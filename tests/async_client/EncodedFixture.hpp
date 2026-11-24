/* Real Model owners and independent Encoded v1 bytes. MIT. */
#ifndef TELEMETRY_TESTS_ASYNC_CLIENT_ENCODED_FIXTURE_HPP
#define TELEMETRY_TESTS_ASYNC_CLIENT_ENCODED_FIXTURE_HPP
#pragma once

#include <integration/encoded/Dispatcher.hpp>
#include <integration/encoded/Backend.hpp>
#include <telemetry/model/Model.hpp>
#include <array>
#include <algorithm>

namespace encoded_fixture {
namespace ts = telemetry;
namespace client = ts::client;
namespace wire = ts::protocol::encoded;
namespace server = ::telemetry::integration::encoded;
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;

struct Config {
	std::uint32_t value;
	bool enabled;
};

struct Big {
	std::array<std::uint32_t, 128> values;
};

struct Device {
	std::array<std::byte, 32> prefix{};
	Config current{42, true};
	std::array<std::byte, 64> suffix{};
	unsigned reads = 0, writes = 0, commands = 0, services = 0;
	ts::WriteResult writeStatus = ts::WriteResult::Applied;
	ts::CommandResult commandStatus = ts::CommandResult::Executed;
	ts::ServiceStatus serviceStatus = ts::ServiceStatus::Ok;

	Config read() noexcept
	{
		++reads;
		return current;
	}

	const Config& borrowed() noexcept
	{
		++reads;
		return current;
	}

	ts::WriteResult write(const Config& value) noexcept
	{
		++writes;
		current = value;
		return writeStatus;
	}

	ts::CommandResult command(const Config& value) noexcept
	{
		++commands;
		current = value;
		return commandStatus;
	}

	ts::CommandResult reset() noexcept
	{
		++commands;
		return commandStatus;
	}

	ts::CommandResult accept(const Config& value) noexcept
	{
		++commands;
		current = value;
		return ts::CommandResult::Accepted;
	}

	ts::ServiceResult<Config> echo(const Config& value) noexcept
	{
		++services;
		return serviceStatus == ts::ServiceStatus::Ok
		           ? ts::ServiceResult<Config>::success(value)
		           : ts::ServiceResult<Config>::failure(serviceStatus);
	}

	ts::ServiceResult<void> ping() noexcept
	{
		++services;
		return serviceStatus == ts::ServiceStatus::Ok
		           ? ts::ServiceResult<void>::success()
		           : ts::ServiceResult<void>::failure(serviceStatus);
	}

	ts::ServiceResult<Config> refuse(const Config&) noexcept
	{
		++services;
		return ts::ServiceResult<Config>::failure(ts::ServiceStatus::Busy);
	}

	Big readBig() noexcept
	{
		++reads;
		return Big{{current.value}};
	}
};

inline Device device;
inline ts::OwnerSlot<Device> optionalOwner;
inline constexpr ts::Model model{ts::group(
    "device", ts::field<&Device::read, &Device::write>("Config", device),
    ts::field<&Device::read>("ReadOnly", device), ts::field<&Device::borrowed>("Borrowed", device),
    ts::field<&Device::readBig>("Big", device),
    ts::field<&Device::read, &Device::write>("Optional", optionalOwner),
    ts::command<&Device::command>("Configure", device),
    ts::command<&Device::reset>("Reset", device), ts::command<&Device::accept>("Accepted", device),
    ts::service<&Device::echo>("Echo", device), ts::service<&Device::ping>("Ping", device),
    ts::service<&Device::refuse>("Busy", device))};
inline constexpr auto modelView = model.view();

inline void put32(Output output, std::size_t offset, std::uint32_t value) noexcept
{
	for (unsigned i = 0; i < 4; ++i)
		output[offset + i] = std::byte((value >> (8 * i)) & 0xffu);
}

// Independent byte fixture: never calls the production frame builder.
inline std::size_t request(Output output, wire::Operation operation, std::uint32_t endpoint,
                           Input payload, std::uint32_t responseBytes, std::uint32_t session = 17,
                           std::uint32_t id = 1) noexcept
{
	std::fill(output.begin(), output.end(), std::byte{0});
	output[0] = std::byte{'T'};
	output[1] = std::byte{'E'};
	output[2] = std::byte{'Q'};
	output[3] = std::byte{'1'};
	output[4] = std::byte{1};
	output[6] = std::byte(static_cast<std::uint8_t>(operation));
	put32(output, 8, session);
	put32(output, 12, id);
	put32(output, 16, endpoint);
	put32(output, 20, static_cast<std::uint32_t>(payload.size()));
	put32(output, 24, responseBytes);
	std::copy(payload.begin(), payload.end(), output.begin() + 32);
	return 32 + payload.size();
}

inline std::array<std::byte, 5> configBytes(std::uint32_t value = 42, bool enabled = true) noexcept
{
	std::array<std::byte, 5> bytes{};
	put32(bytes, 0, value);
	bytes[4] = std::byte(enabled ? 1 : 0);
	return bytes;
}

inline unsigned calls() noexcept
{
	return device.reads + device.writes + device.commands + device.services;
}

struct Harness {
	std::array<std::byte, 1024> scratch{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, 1024> tx{}, result{}, port{}, reply{};
	std::uint32_t now = 0;
	::telemetry::integration::encoded::EncodedBackend backend{
	    tx, result,
	    client::Clock{this,
	                  +[](void* self) noexcept {
		                  return static_cast<Harness*>(self)->now;
	                  }},
	    10};
};
} // namespace encoded_fixture
#endif // TELEMETRY_TESTS_ASYNC_CLIENT_ENCODED_FIXTURE_HPP

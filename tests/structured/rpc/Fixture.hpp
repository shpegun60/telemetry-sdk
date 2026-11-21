/* Counted native owners and independent RpcMailbox packet fixtures. MIT. */
#ifndef TELEMETRY_TESTS_STRUCTURED_RPC_FIXTURE_HPP
#define TELEMETRY_TESTS_STRUCTURED_RPC_FIXTURE_HPP
#pragma once

#include <integration/telemetry/v3/RpcMailbox.hpp>
#include <resource/File.hpp>
#include <telemetry/model/Model.hpp>
#include <telemetry/model/Adapter.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <type_traits>

namespace rpc_fixture {
namespace ts = telemetry;
namespace rpc = resource::telemetry::v3;
using resource::Input;
using resource::Output;
using R = resource::Status;
using Op = ::telemetry::protocol::rpc::RpcOperation;
using State = ::telemetry::protocol::rpc::RpcState;
using W = ts::WriteResult;
using C = ts::CommandResult;
using S = ts::ServiceStatus;

struct Config {
	std::uint32_t value;
	bool enabled;
};

struct Big {
	std::array<std::uint32_t, 1024> words;
};

struct Golden {
	std::array<std::uint8_t, 96> bytes;
};

struct Device {
	Config current{42, true};
	unsigned reads = 0, writes = 0, commands = 0, services = 0;
	W writeStatus = W::Applied;
	C commandStatus = C::Executed;
	S serviceStatus = S::Ok;
	void (*hook)() noexcept = nullptr;

	void visit() noexcept
	{
		if (hook)
			hook();
	}

	Config read() noexcept
	{
		++reads;
		visit();
		return current;
	}

	const Config& borrowed() noexcept
	{
		++reads;
		visit();
		return current;
	}

	W write(const Config& value) noexcept
	{
		++writes;
		visit();
		current = value;
		return writeStatus;
	}

	C configure(const Config& value) noexcept
	{
		++commands;
		visit();
		current = value;
		return commandStatus;
	}

	C reset() noexcept
	{
		++commands;
		visit();
		return commandStatus;
	}

	C golden(const Golden&) noexcept
	{
		++commands;
		visit();
		return commandStatus;
	}

	ts::ServiceResult<Config> echo(const Config& value) noexcept
	{
		++services;
		visit();
		return serviceStatus == S::Ok ? ts::ServiceResult<Config>::success(value)
		                              : ts::ServiceResult<Config>::failure(serviceStatus);
	}

	ts::ServiceResult<void> ping() noexcept
	{
		++services;
		visit();
		return serviceStatus == S::Ok ? ts::ServiceResult<void>::success()
		                              : ts::ServiceResult<void>::failure(serviceStatus);
	}

	ts::ServiceResult<Big> large(const Config& value) noexcept
	{
		++services;
		visit();
		current = value;
		if (serviceStatus != S::Ok)
			return ts::ServiceResult<Big>::failure(serviceStatus);
		return ts::ServiceResult<Big>::successFrom([&]() noexcept {
			return Big{{value.value}};
		});
	}

	Big readBig() noexcept
	{
		++reads;
		visit();
		return Big{{current.value}};
	}

	W writeBig(const Big& value) noexcept
	{
		++writes;
		visit();
		current.value = value.words.back();
		return writeStatus;
	}

	C commandBig(const Big& value) noexcept
	{
		++commands;
		visit();
		current.value = value.words.back();
		return commandStatus;
	}
};

inline Device device;
inline ts::OwnerSlot<Device> slot;
// Two groups: device holds every family, golden three more Commands. Names
// are unique within a group across its families; IDs are group:position.
inline constexpr ts::Model model{
    ts::group("device", ts::field<&Device::read, &Device::write>("Config", device),
              ts::field<&Device::read>("ReadOnly", device),
              ts::field<&Device::read, &Device::write>("Optional", slot),
              ts::field<&Device::readBig, &Device::writeBig>("Big", device),
              ts::field<&Device::borrowed>("Borrowed", device),
              ts::command<&Device::configure>("Configure", device),
              ts::command<&Device::reset>("Reset", device),
              ts::command<&Device::configure>("OptionalConfigure", slot),
              ts::command<&Device::commandBig>("BigCommand", device),
              ts::service<&Device::echo>("Echo", device),
              ts::service<&Device::ping>("Ping", device),
              ts::service<&Device::echo>("OptionalEcho", slot),
              ts::service<&Device::large>("Large", device)),
    ts::group("golden", ts::command<&Device::reset>("Zero", device),
              ts::command<&Device::reset>("One", device),
              ts::command<&Device::golden>("Golden", device))};
static_assert(ts::idOf<model, "device/Borrowed">() == 4 &&
              ts::idOf<model, "device/BigCommand">() == 3 &&
              ts::idOf<model, "golden/Golden">() == 0x00010002u &&
              ts::idOf<model, "device/Large">() == 3);
inline constexpr auto view = model.view();

template<class U>
inline void put(Output bytes, std::size_t offset, U value) noexcept
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

inline resource::Cursor cursor(std::uint32_t tx, std::uint32_t offset) noexcept
{
	return (std::uint64_t(tx) << 32) | offset;
}

inline void header(Output bytes, Op op, std::uint32_t id, std::uint32_t payload,
                   std::uint32_t tx) noexcept
{
	std::fill_n(bytes.begin(), 24, std::byte{0});
	bytes[0] = std::byte{'T'};
	bytes[1] = std::byte{'R'};
	bytes[2] = std::byte{'P'};
	bytes[3] = std::byte{'C'};
	bytes[4] = std::byte{1};
	put(bytes, 8, tx);
	put(bytes, 12, id);
	put(bytes, 16, payload);
	bytes[20] = static_cast<std::byte>(op);
}

template<class T>
inline auto packet(Op op, std::uint32_t id, const T& value, std::uint32_t tx = 1) noexcept
{
	std::array<std::byte, 24 + ts::wireSize<T>> bytes{};
	header(bytes, op, id, ts::wireSize<T>, tx);
	(void)ts::encode(value, Output{bytes}.subspan(24));
	return bytes;
}

inline auto emptyPacket(Op op, std::uint32_t id, std::uint32_t tx = 1) noexcept
{
	std::array<std::byte, 24> bytes{};
	header(bytes, op, id, 0, tx);
	return bytes;
}

inline unsigned callbacks() noexcept
{
	return device.reads + device.writes + device.commands + device.services;
}

template<std::size_t N>
struct Guarded {
	std::array<std::byte, N + 2> bytes{};

	Guarded() noexcept
	{
		bytes.fill(std::byte{0xcd});
	}

	Output output() noexcept
	{
		return Output{bytes}.subspan(1, N);
	}

	bool intact() const noexcept
	{
		return bytes.front() == std::byte{0xcd} && bytes.back() == std::byte{0xcd};
	}
};

template<std::size_t Request = 24 + ts::wireSize<Big>, std::size_t Result = ts::wireSize<Big>,
         std::size_t Scratch = model.maxScratch()>
struct Harness {
	Guarded<Request> request;
	Guarded<Result> result;
	Guarded<Scratch> scratch;
	ts::Workspace workspace{scratch.output()};
	rpc::RpcMailbox mailbox{view, workspace, request.output(), result.output()};

	bool intact() const noexcept
	{
		return request.intact() && result.intact() && scratch.intact();
	}
};

static_assert(resource::Provider<rpc::RpcMailbox>);
static_assert(!std::is_copy_constructible_v<rpc::RpcMailbox>);
static_assert(!std::is_move_constructible_v<rpc::RpcMailbox>);
} // namespace rpc_fixture
#endif

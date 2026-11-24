/* Actual native Model for typed static Mixed Batch qualification. MIT. */
#ifndef TELEMETRY_TESTS_BATCH_STATIC_MODEL_HPP
#define TELEMETRY_TESTS_BATCH_STATIC_MODEL_HPP
#pragma once
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <array>
#include <bit>
#include <cstdint>

namespace static_batch_fixture {
namespace ts = telemetry;

enum class Mode : std::int16_t {
	Reverse = -7,
	Forward = 12
};

struct Empty {};

struct FlagReply {
	bool value;
};

struct Data {
	std::uint32_t number;
	bool enabled;
	std::array<std::uint16_t, 3> gains;
	Mode mode;
	std::uint64_t large;
	std::int64_t negative;
	float f32;
	double f64;
};

inline Data initial(std::uint32_t number = 42) noexcept
{
	return {number,
	        true,
	        {0, 65535, 17},
	        static_cast<Mode>(-123),
	        UINT64_MAX,
	        INT64_MIN,
	        std::bit_cast<float>(0xff812345u),
	        std::bit_cast<double>(UINT64_C(0x8000000000000000))};
}

struct Event {
	unsigned operation, group, index;
	std::uint32_t number;
	friend bool operator==(const Event&, const Event&) = default;
};

inline std::array<Event, 128> trace{};
inline unsigned traceCount = 0;

struct Device {
	unsigned group;
	Data current = initial();
	unsigned reads = 0, writes = 0, commands = 0, services = 0;

	void note(unsigned operation, unsigned index, std::uint32_t number) noexcept
	{
		if (traceCount == trace.size())
			std::abort();
		trace[traceCount++] = {operation, group, index, number};
	}

	Data read() noexcept
	{
		++reads;
		note(1, 0, current.number);
		return current;
	}

	ts::WriteResult write(const Data& value) noexcept
	{
		++writes;
		note(2, 0, value.number);
		current = value;
		if (current.number > 1000)
			current.number = 1000;
		return ts::WriteResult::Applied;
	}

	std::uint32_t counter() noexcept
	{
		++reads;
		note(1, 1, current.number);
		return current.number;
	}

	Data readOnly() noexcept
	{
		++reads;
		note(1, 2, current.number);
		return current;
	}

	Empty empty() noexcept
	{
		++reads;
		note(1, 3, 0);
		return {};
	}

	bool flag() noexcept
	{
		++reads;
		note(1, 4, 1);
		return true;
	}

	ts::CommandResult configure(const Data& value) noexcept
	{
		++commands;
		note(3, 0, value.number);
		current = value;
		current.number ^= 0x55u;
		return ts::CommandResult::Executed;
	}

	ts::CommandResult reset() noexcept
	{
		++commands;
		note(3, 1, 0);
		current = initial(0);
		return ts::CommandResult::Executed;
	}

	ts::CommandResult accepted(const Data& value) noexcept
	{
		++commands;
		note(3, 2, value.number);
		return ts::CommandResult::Accepted;
	}

	ts::CommandResult emptyArgument(const Empty&) noexcept
	{
		++commands;
		note(3, 3, 0);
		return ts::CommandResult::Executed;
	}

	ts::CommandResult reject(const Data& value) noexcept
	{
		++commands;
		note(3, 4, value.number);
		return ts::CommandResult::InvalidValue;
	}

	ts::ServiceResult<Data> echo(const Data& value) noexcept
	{
		++services;
		note(4, 0, value.number);
		return ts::ServiceResult<Data>::success(value);
	}

	ts::ServiceResult<void> ping() noexcept
	{
		++services;
		note(4, 1, 0);
		return ts::ServiceResult<void>::success();
	}

	ts::ServiceResult<Empty> emptyService() noexcept
	{
		++services;
		note(4, 2, 0);
		return ts::ServiceResult<Empty>::success({});
	}

	ts::ServiceResult<Data> busy(const Data& value) noexcept
	{
		++services;
		note(4, 3, value.number);
		return ts::ServiceResult<Data>::failure(ts::ServiceStatus::Busy);
	}

	ts::ServiceResult<void> consume(const Data& value) noexcept
	{
		++services;
		note(4, 4, value.number);
		return ts::ServiceResult<void>::success();
	}

	ts::ServiceResult<Data> emptyRequest(const Empty&) noexcept
	{
		++services;
		note(4, 5, current.number);
		return ts::ServiceResult<Data>::success(current);
	}

	ts::ServiceResult<FlagReply> flagService() noexcept
	{
		++services;
		note(4, 6, 1);
		return ts::ServiceResult<FlagReply>::success({true});
	}
};

inline Device decoy{0}, device{1};
inline ts::OwnerSlot<Device> absent;

inline void reset() noexcept
{
	decoy = Device{0, initial(700)};
	device = Device{1};
	trace.fill({});
	traceCount = 0;
}

// One group per owner: its Fields, Commands and Services, names unique in the group.
constexpr auto branch(const char* name, Device& owner) noexcept
{
	return ts::group(
	    name, ts::field<&Device::read, &Device::write>("Config", owner),
	    ts::field<&Device::counter>("Counter", owner),
	    ts::field<&Device::readOnly>("ReadOnly", owner), ts::field<&Device::empty>("Empty", owner),
	    ts::field<&Device::flag>("Bool", owner), ts::field<&Device::read>("Unavailable", absent),
	    ts::command<&Device::configure>("Configure", owner),
	    ts::command<&Device::reset>("Reset", owner),
	    ts::command<&Device::accepted>("Accepted", owner),
	    ts::command<&Device::emptyArgument>("EmptyArgument", owner),
	    ts::command<&Device::reject>("Reject", owner), ts::service<&Device::echo>("Echo", owner),
	    ts::service<&Device::ping>("Ping", owner),
	    ts::service<&Device::emptyService>("EmptyService", owner),
	    ts::service<&Device::busy>("Busy", owner), ts::service<&Device::consume>("Consume", owner),
	    ts::service<&Device::emptyRequest>("EmptyRequest", owner),
	    ts::service<&Device::flagService>("Flag", owner));
}

// The decoy group first, so control's IDs carry group 1 in every family.
inline constexpr ts::Model model{branch("decoy", decoy), branch("control", device)};
inline constexpr ts::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = ts::descriptor::packDescriptor<descriptor>();
} // namespace static_batch_fixture
#endif

/*
 * @file LargeEndpoints.cpp
 * @brief 4 KiB native objects are constructed/decoded in caller scratch.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/model/Model.hpp>

#if !defined(__arm__)
// Any unexpected dynamic allocation fails the host test immediately.
void* operator new(std::size_t)
{
	std::abort();
}

void* operator new[](std::size_t)
{
	std::abort();
}

void operator delete(void*) noexcept
{
	std::abort();
}

void operator delete[](void*) noexcept
{
	std::abort();
}

void operator delete(void*, std::size_t) noexcept
{
	std::abort();
}

void operator delete[](void*, std::size_t) noexcept
{
	std::abort();
}
#endif

namespace ts = telemetry;

namespace large {
struct Value {
	std::array<std::uint8_t, 4096> data;
};

inline int reads = 0, writes = 0, calls = 0;

Value get() noexcept
{
	++reads;
	Value value{};
	value.data.front() = 17;
	value.data.back() = 39;
	return value;
}

telemetry::WriteResult set(const Value& value) noexcept
{
	++writes;
	return value.data.front() == 17 && value.data.back() == 39
	           ? telemetry::WriteResult::Applied
	           : telemetry::WriteResult::InvalidValue;
}

telemetry::CommandResult command(const Value& value) noexcept
{
	++calls;
	return value.data.front() == 17 && value.data.back() == 39
	           ? telemetry::CommandResult::Executed
	           : telemetry::CommandResult::InvalidValue;
}

inline constexpr ts::Model model{
    ts::group("large", ts::field<&get, &set>("value"), ts::command<&command>("apply"))};
static_assert(model.maxScratch() == 4096);
static_assert(model.maxFieldWireSize() == 4096);
} // namespace large

int main()
{
	// Storage is static so the measured frames isolate dispatch and codec.
	static std::array<std::byte, 4096> bytes{}, scratch{};
	ts::Workspace workspace{scratch};
	ts::Workspace shortWorkspace{std::span{scratch}.first(4095)};
	if (large::model.fieldIndex().readEncoded(0, bytes, shortWorkspace).dispatch !=
	        ts::DispatchStatus::WorkspaceTooSmall ||
	    large::reads != 0)
		return 1;
	const auto read = large::model.fieldIndex().readEncoded(0, bytes, workspace);
	if (read.dispatch != ts::DispatchStatus::Ok || read.written != 4096 ||
	    bytes.front() != std::byte{17} || bytes.back() != std::byte{39} || large::reads != 1)
		return 2;
	const auto write = large::model.fieldIndex().writeEncoded(0, bytes, workspace);
	const auto call = large::model.commandIndex().executeEncoded(0, bytes, workspace);
	if (write.dispatch != ts::DispatchStatus::Ok ||
	    write.endpointStatus != telemetry::WriteResult::Applied ||
	    call.dispatch != ts::DispatchStatus::Ok ||
	    call.endpointStatus != telemetry::CommandResult::Executed || large::writes != 1 ||
	    large::calls != 1 || workspace.used() != 0)
		return 3;
	if (large::model.fieldIndex().writeEncoded(0, bytes, shortWorkspace).dispatch !=
	        ts::DispatchStatus::WorkspaceTooSmall ||
	    large::model.commandIndex().executeEncoded(0, bytes, shortWorkspace).dispatch !=
	        ts::DispatchStatus::WorkspaceTooSmall ||
	    large::writes != 1 || large::calls != 1)
		return 4;
	return 0;
}

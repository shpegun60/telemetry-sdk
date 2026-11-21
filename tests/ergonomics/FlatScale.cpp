/*
 * @file FlatScale.cpp
 * @brief Manual, nested native and flat runtime helpers at 128 and 256 targets.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <cassert>
#include <cstdio>

#ifndef TARGETS
#define TARGETS 128
#endif

namespace flat_scale {
namespace ts = telemetry;

struct Request {
	std::uint32_t value;
};

struct Reply {
	std::uint32_t value;
};

std::uint32_t commandCalls = 0, serviceCalls = 0;
volatile std::uint32_t sink = 0;

template<std::size_t I>
ts::CommandResult command(const Request& request) noexcept
{
	++commandCalls;
	sink = request.value + static_cast<std::uint32_t>(I);
	return ts::CommandResult::Executed;
}

template<std::size_t I>
Reply service(const Request& request) noexcept
{
	++serviceCalls;
	return {request.value + static_cast<std::uint32_t>(I)};
}

template<std::size_t Group, std::size_t... I>
constexpr auto makeCommands(std::index_sequence<I...>)
{
	return ts::CommandTable{ts::command<&command<Group * 32 + I>>("Entry")...};
}

template<std::size_t Group, std::size_t... I>
constexpr auto makeServices(std::index_sequence<I...>)
{
	return ts::ServiceTable{ts::service<&service<Group * 32 + I>>("Entry")...};
}

// Use the documented grouped layout and preserve the older scale fixture.
template<std::size_t Group>
inline constexpr auto localCommands = makeCommands<Group>(std::make_index_sequence<32>{});

template<std::size_t Group>
inline constexpr auto localServices = makeServices<Group>(std::make_index_sequence<32>{});

template<std::size_t... G>
constexpr auto commandGroups(std::index_sequence<G...>)
{
	return ts::testing::CommandCatalogTable{ts::testing::group("Group", localCommands<G>)...};
}

template<std::size_t... G>
constexpr auto serviceGroups(std::index_sequence<G...>)
{
	return ts::testing::ServiceCatalogTable{ts::testing::group("Group", localServices<G>)...};
}

static_assert(TARGETS == 128 || TARGETS == 256);
inline constexpr auto commands = commandGroups(std::make_index_sequence<TARGETS / 32>{});
inline constexpr auto services = serviceGroups(std::make_index_sequence<TARGETS / 32>{});

struct CommandVisitor {
	const Request& request;
	ts::CommandResult result = ts::CommandResult::NotFound;

	template<class Definition>
	void operator()(const Definition& definition)
	{
		result = definition.call(request);
	}
};

struct ServiceVisitor {
	const Request& request;
	std::uint32_t value = 0;

	template<class Definition>
	void operator()(const Definition& definition)
	{
		auto result = definition.call(request);
		value = result.hasValue() ? result.value().value : 0;
	}
};
} // namespace flat_scale

extern "C" telemetry::CommandResult flat_manual_command(std::uint32_t id,
                                                        const flat_scale::Request& request)
{
	flat_scale::CommandVisitor visitor{request};
	(void)flat_scale::commands.visit(id, visitor);
	return visitor.result;
}

extern "C" telemetry::CommandResult flat_native_command(std::uint32_t id,
                                                        const flat_scale::Request& request)
{
	auto result = flat_scale::commands.callAs(id, request);
	return result.hasValue() ? result.value() : telemetry::CommandResult::NotFound;
}

extern "C" telemetry::CommandCallStatus flat_api_command(std::uint32_t id,
                                                         const flat_scale::Request& request)
{
	return flat_scale::commands.call(id, request);
}

extern "C" std::uint32_t flat_manual_service(std::uint32_t id, const flat_scale::Request& request)
{
	flat_scale::ServiceVisitor visitor{request};
	(void)flat_scale::services.visit(id, visitor);
	return visitor.value;
}

extern "C" std::uint32_t flat_native_service(std::uint32_t id, const flat_scale::Request& request)
{
	auto result =
	    flat_scale::services.callAs<telemetry::ServiceResult<flat_scale::Reply>>(id, request);
	return result.hasValue() && result.value().hasValue() ? result.value().value().value : 0;
}

extern "C" std::uint32_t flat_api_service(std::uint32_t id, const flat_scale::Request& request)
{
	auto result = flat_scale::services.callAs<flat_scale::Reply>(id, request);
	return result.hasValue() ? result.value().value : 0;
}

int main()
{
	unsigned checks = 0;
#define CHECK(condition)                                                                           \
	do {                                                                                           \
		++checks;                                                                                  \
		assert((condition));                                                                       \
	} while (false)
	for (std::uint32_t id = 0; id < TARGETS; ++id) {
		const flat_scale::Request request{19};
		const auto packed = telemetry::detail::makeId(id / 32, id % 32);
		const auto commandBefore = flat_scale::commandCalls;
		CHECK(flat_manual_command(packed, request) == telemetry::CommandResult::Executed);
		CHECK(flat_scale::commandCalls == commandBefore + 1);
		CHECK(flat_scale::sink == id + request.value);
		CHECK(flat_native_command(packed, request) == telemetry::CommandResult::Executed);
		CHECK(flat_scale::commandCalls == commandBefore + 2);
		CHECK(flat_scale::sink == id + request.value);
		CHECK(flat_api_command(packed, request) == telemetry::CommandCallStatus::Executed);
		CHECK(flat_scale::commandCalls == commandBefore + 3);
		CHECK(flat_scale::sink == id + request.value);
		const auto serviceBefore = flat_scale::serviceCalls;
		CHECK(flat_manual_service(packed, request) == id + request.value);
		CHECK(flat_scale::serviceCalls == serviceBefore + 1);
		CHECK(flat_native_service(packed, request) == id + request.value);
		CHECK(flat_scale::serviceCalls == serviceBefore + 2);
		CHECK(flat_api_service(packed, request) == id + request.value);
		CHECK(flat_scale::serviceCalls == serviceBefore + 3);
	}
	const auto invalid = telemetry::detail::makeId(TARGETS / 32, 0);
	const auto commandBefore = flat_scale::commandCalls, serviceBefore = flat_scale::serviceCalls;
	CHECK(flat_manual_command(invalid, {}) == telemetry::CommandResult::NotFound);
	CHECK(flat_native_command(invalid, {}) == telemetry::CommandResult::NotFound);
	CHECK(flat_api_command(invalid, {}) == telemetry::CommandCallStatus::NotFound);
	CHECK(flat_manual_service(invalid, {}) == 0);
	CHECK(flat_native_service(invalid, {}) == 0);
	CHECK(flat_api_service(invalid, {}) == 0);
	CHECK(flat_scale::commandCalls == commandBefore);
	CHECK(flat_scale::serviceCalls == serviceBefore);
	CHECK(flat_scale::commands.call(invalid, flat_scale::Request{}) ==
	      telemetry::CommandCallStatus::NotFound);
	CHECK(flat_scale::services.callAs<flat_scale::Reply>(invalid, flat_scale::Request{}).status() ==
	      telemetry::ServiceCallStatus::NotFound);
	CHECK(flat_scale::commandCalls == commandBefore);
	CHECK(flat_scale::serviceCalls == serviceBefore);
	CHECK(flat_scale::commandCalls == TARGETS * 3);
	CHECK(flat_scale::serviceCalls == TARGETS * 3);
	std::printf("FlatScale: %u checks passed (%u targets)\n", checks, TARGETS);
}

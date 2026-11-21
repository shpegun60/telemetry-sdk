/*
 * @file NativeScale.cpp
 * @brief Named visitor versus native helpers at 128 and 256 positional targets.
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
namespace scale {
namespace ts = telemetry;

struct Request {
	std::uint32_t value;
};

struct Reply {
	std::uint32_t value;
};

volatile std::uint32_t sink = 0;

template<std::size_t I>
ts::CommandResult command(const Request& request) noexcept
{
	sink = request.value + static_cast<std::uint32_t>(I);
	return ts::CommandResult::Executed;
}

template<std::size_t I>
Reply service(const Request& request) noexcept
{
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

// Follow the documented large-project layout. A giant tuple can exceed PE
// symbol-table budgets even before native convenience dispatch is added.
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

static_assert(TARGETS % 32 == 0);
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
		value = definition.call(request).value().value;
	}
};
} // namespace scale

extern "C" telemetry::CommandResult manual_command(std::uint32_t id, const scale::Request& request)
{
	scale::CommandVisitor visitor{request};
	(void)scale::commands.visit(id, visitor);
	return visitor.result;
}

extern "C" telemetry::CommandResult convenience_command(std::uint32_t id,
                                                        const scale::Request& request)
{
	auto result = scale::commands.callAs(id, request);
	return result.hasValue() ? result.value() : telemetry::CommandResult::NotFound;
}

extern "C" std::uint32_t manual_service(std::uint32_t id, const scale::Request& request)
{
	scale::ServiceVisitor visitor{request};
	(void)scale::services.visit(id, visitor);
	return visitor.value;
}

extern "C" std::uint32_t convenience_service(std::uint32_t id, const scale::Request& request)
{
	auto result = scale::services.callAs<telemetry::ServiceResult<scale::Reply>>(id, request);
	return result.hasValue() ? result.value().value().value : 0;
}

int main()
{
	for (std::uint32_t id = 0; id < TARGETS; ++id) {
		const scale::Request request{19};
		const auto packed = telemetry::detail::makeId(id / 32, id % 32);
		assert(manual_command(packed, request) == telemetry::CommandResult::Executed);
		assert(convenience_command(packed, request) == telemetry::CommandResult::Executed);
		assert(scale::sink == id + request.value);
		assert(manual_service(packed, request) == id + request.value);
		assert(convenience_service(packed, request) == id + request.value);
	}
	assert(manual_command(TARGETS, {}) == telemetry::CommandResult::NotFound);
	assert(convenience_command(TARGETS, {}) == telemetry::CommandResult::NotFound);
	assert(manual_service(TARGETS, {}) == 0);
	assert(convenience_service(TARGETS, {}) == 0);
	std::printf("NativeScale: %u checks passed (%u targets)\n", TARGETS * 5 + 4, TARGETS);
}

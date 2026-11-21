/*
 * @file NativeNegative.cpp
 * @brief Reject ambiguous native result types, volatile inputs and narrowing bypasses.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>

struct Request {
	unsigned value;
};

struct Reply {
	unsigned value;
};

telemetry::CommandResult command(const Request&) noexcept
{
	return telemetry::CommandResult::Executed;
}

Reply service(const Request&) noexcept
{
	return {};
}

inline constexpr telemetry::CommandTable commands{telemetry::command<&command>("Command")};
inline constexpr telemetry::ServiceTable services{telemetry::service<&service>("Service")};
inline constexpr telemetry::testing::CommandCatalogTable commandCatalog{
    telemetry::testing::group("Control", commands)};
inline constexpr telemetry::testing::ServiceCatalogTable serviceCatalog{
    telemetry::testing::group("Control", services)};
using Result = telemetry::ServiceResult<Reply>;

void test()
{
#if CASE == 1
	(void)commands.callAs<unsigned short>(65537, Request{});
#elif CASE == 2
	(void)commandCatalog.callAs<unsigned short>(65537, Request{});
#elif CASE == 3
	(void)services.callAs<Result, unsigned short>(65537, Request{});
#elif CASE == 4
	(void)serviceCatalog.callAs<Result, unsigned short>(65537, Request{});
#elif CASE == 5
	volatile Request request{};
	(void)commands.callAs(0, request);
#elif CASE == 6
	volatile Request request{};
	(void)services.callAs<Result>(0, request);
#elif CASE == 7
	(void)std::move(commands).callAs(0, Request{});
#elif CASE == 8
	(void)std::move(services).callAs<Result>(0, Request{});
#elif CASE == 9
	(void)std::move(commandCatalog).callAs(0, Request{});
#elif CASE == 10
	(void)std::move(serviceCatalog).callAs<Result>(0, Request{});
#elif CASE == 11
	// The payload form callAs<Reply>() is valid; raw-pointer output stays invalid.
	(void)services.callAs<Reply*>(0, Request{});
#elif CASE == 12
	(void)services.callAs<const Result>(0, Request{});
#elif CASE == 13
	(void)services.callAs<Result&>(0, Request{});
#elif CASE == 14
	(void)telemetry::NativeCallResult<Result>::successFrom([] {
		return Reply{};
	});
#endif
}

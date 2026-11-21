/*
 * @file FlatNegative.cpp
 * @brief Intended rejection probes for flat native lifetime, exact types and ID guards.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <utility>

namespace ts = telemetry;

struct Request {
	unsigned value;
};

struct Reply {
	unsigned value;
};

ts::CommandResult command(const Request&) noexcept
{
	return ts::CommandResult::Executed;
}

Reply service(const Request&) noexcept
{
	return {};
}

inline constexpr ts::CommandTable commands{ts::command<&command>("Command")};
inline constexpr ts::ServiceTable services{ts::service<&service>("Service")};
inline constexpr ts::testing::CommandCatalogTable commandCatalog{
    ts::testing::group("Control", commands)};
inline constexpr ts::testing::ServiceCatalogTable serviceCatalog{
    ts::testing::group("Control", services)};
using Owned = ts::ServiceCallResult<Reply>;
using Borrowed = ts::BorrowedServiceCallResult<Reply>;

void test()
{
#if CASE == 1
	(void)commands.call<unsigned short>(65537, Request{});
#elif CASE == 2
	(void)commandCatalog.call<unsigned short>(65537, Request{});
#elif CASE == 3
	(void)services.callAs<Reply, unsigned short>(65537, Request{});
#elif CASE == 4
	(void)serviceCatalog.callAs<Reply, unsigned short>(65537, Request{});
#elif CASE == 5
	(void)services.callBorrowed<Reply, unsigned short>(65537, Request{});
#elif CASE == 6
	(void)serviceCatalog.callBorrowed<Reply, unsigned short>(65537, Request{});
#elif CASE == 7
	(void)commands.call<unsigned short>(65537);
#elif CASE == 8
	(void)commandCatalog.call<unsigned short>(65537);
#elif CASE == 9
	(void)services.callAs<Reply, unsigned short>(65537);
#elif CASE == 10
	(void)serviceCatalog.callAs<Reply, unsigned short>(65537);
#elif CASE == 11
	(void)services.callBorrowed<Reply, unsigned short>(65537);
#elif CASE == 12
	(void)serviceCatalog.callBorrowed<Reply, unsigned short>(65537);
#elif CASE == 13
	volatile Request request{};
	(void)commands.call(0, request);
#elif CASE == 14
	volatile Request request{};
	(void)commandCatalog.call(0, request);
#elif CASE == 15
	volatile Request request{};
	(void)services.callAs<Reply>(0, request);
#elif CASE == 16
	volatile Request request{};
	(void)serviceCatalog.callAs<Reply>(0, request);
#elif CASE == 17
	volatile Request request{};
	(void)services.callBorrowed<Reply>(0, request);
#elif CASE == 18
	volatile Request request{};
	(void)serviceCatalog.callBorrowed<Reply>(0, request);
#elif CASE == 19
	(void)std::move(commands).call(0, Request{});
#elif CASE == 20
	(void)std::move(commandCatalog).call(0, Request{});
#elif CASE == 21
	(void)std::move(services).callAs<Reply>(0, Request{});
#elif CASE == 22
	(void)std::move(serviceCatalog).callAs<Reply>(0, Request{});
#elif CASE == 23
	(void)std::move(services).callBorrowed<Reply>(0, Request{});
#elif CASE == 24
	(void)std::move(serviceCatalog).callBorrowed<Reply>(0, Request{});
#elif CASE == 25
	(void)services.callAs<const Reply>(0, Request{});
#elif CASE == 26
	(void)services.callAs<volatile Reply>(0, Request{});
#elif CASE == 27
	(void)services.callAs<Reply&>(0, Request{});
#elif CASE == 28
	(void)services.callBorrowed<const Reply>(0, Request{});
#elif CASE == 29
	(void)services.callBorrowed<volatile Reply>(0, Request{});
#elif CASE == 30
	(void)services.callBorrowed<Reply&>(0, Request{});
#elif CASE == 31
	(void)services.callAs<Reply*>(0, Request{});
#elif CASE == 32
	(void)services.callBorrowed<void>(0, Request{});
#elif CASE == 33
	(void)serviceCatalog.callBorrowed<Reply*>(0, Request{});
#elif CASE == 34
	Borrowed borrowed = services.callAs<Reply>(0, Request{});
	(void)borrowed;
#elif CASE == 35
	bool success = services.callAs<Reply>(0, Request{});
	(void)success;
#elif CASE == 36
	bool success = services.callBorrowed<Reply>(0, Request{});
	(void)success;
#elif CASE == 37
	services.callBorrowed<Reply>(0, Request{}).value().value = 1;
#elif CASE == 38
	(void)services.callAs<void>(0, Request{}).valueOrNull();
#elif CASE == 39
	(void)Owned::fromNative([] {
		return Reply{};
	});
#elif CASE == 40
	(void)Borrowed::fromNative([] {
		return Reply{};
	});
#elif CASE == 41
	(void)sizeof(ts::ServiceCallResult<const Reply>);
#elif CASE == 42
	(void)sizeof(ts::BorrowedServiceCallResult<const Reply>);
#elif CASE == 43
	(void)sizeof(ts::BorrowedServiceCallResult<void>);
#elif CASE == 44
	auto native = services.callAs<ts::ServiceResult<Reply>>(0, Request{});
	(void)Owned::fromNative([&]() -> decltype(native)& {
		return native;
	});
#elif CASE == 45
	(void)Borrowed::fromNative([] {
		return ts::NativeCallResult<ts::ServiceResult<Reply>>::failure(
		    ts::NativeCallStatus::NotFound);
	});
#elif CASE == 46
	const unsigned id = 0;
	(void)commands.call(&id, Request{});
#elif CASE == 47
	(void)services.callAs<Reply>(0.0, Request{});
#elif CASE == 48
	(void)serviceCatalog.callBorrowed<Reply, Request>(0, Request{});
#elif CASE == 49
	(void)std::move(commands).call(0);
#elif CASE == 50
	(void)std::move(commandCatalog).call(0);
#elif CASE == 51
	(void)std::move(services).callAs<void>(0);
#elif CASE == 52
	(void)std::move(serviceCatalog).callAs<void>(0);
#elif CASE == 53
	(void)std::move(services).callBorrowed<Reply>(0);
#elif CASE == 54
	(void)std::move(serviceCatalog).callBorrowed<Reply>(0);
#elif CASE == 55
	enum class Local {
		Zero = 0
	};
	(void)serviceCatalog.callAs<Reply>(Local::Zero, Request{});
#elif CASE == 56
	enum class Local {
		Zero = 0
	};
	(void)serviceCatalog.callBorrowed<Reply>(Local::Zero, Request{});
#elif CASE == 57
	enum class Local {
		Zero = 0
	};
	(void)commandCatalog.call(Local::Zero, Request{});
#elif CASE == 58
	(void)services.callAs<Reply>(0, Request{}).valueOrNull();
#elif CASE == 59
	(void)serviceCatalog.callAs<Reply>(0, Request{}).valueOrNull();
#elif CASE == 60
	const auto result = services.callAs<Reply>(0, Request{});
	(void)std::move(result).valueOrNull();
#elif CASE == 61
	// Preserve the return type's const qualification on the temporary itself.
	(void)[]()->const Owned
	{
		return serviceCatalog.callAs<Reply>(0, Request{});
	}
	().valueOrNull();
#endif
}

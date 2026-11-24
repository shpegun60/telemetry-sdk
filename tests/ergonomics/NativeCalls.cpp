/*
 * @file NativeCalls.cpp
 * @brief Runtime native selection, endpoint statuses and direct result lifetime.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <telemetry/result/NativeCallResult.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <type_traits>

namespace ts = telemetry;

namespace {
unsigned checks = 0;
#define CHECK(condition)                                                                           \
	do {                                                                                           \
		++checks;                                                                                  \
		assert((condition));                                                                       \
	} while (false)

struct Request {
	std::uint32_t value;
};

struct Other {
	std::uint32_t value;
};

struct Reply {
	std::uint32_t value;
};

struct Big {
	std::array<std::uint32_t, 1024> values;
};

unsigned calls = 0;
Reply borrowed{17};

ts::CommandResult configure(const Request& request) noexcept
{
	++calls;
	return request.value == 0 ? ts::CommandResult::Busy : ts::CommandResult::Executed;
}

ts::CommandResult reset() noexcept
{
	++calls;
	return ts::CommandResult::Accepted;
}

ts::ServiceResult<Reply> query(const Request& request) noexcept
{
	++calls;
	if (request.value == 0)
		return ts::ServiceResult<Reply>::failure(ts::ServiceStatus::Busy);
	return ts::ServiceResult<Reply>::success({request.value});
}

const Reply& borrow(const Request&) noexcept
{
	++calls;
	return borrowed;
}

void notify() noexcept
{
	++calls;
}

ts::ServiceResult<Big> large(const Request& request) noexcept
{
	++calls;
	return ts::ServiceResult<Big>::successFrom([&]() noexcept -> Big {
		return Big{{request.value}};
	});
}

ts::FunctionSlot<ts::CommandResult(const Request&) noexcept> commandSlot;
ts::FunctionSlot<ts::ServiceResult<Reply>(const Request&) noexcept> serviceSlot;
const ts::CommandTable commands{ts::command<&configure>("Configure"), ts::command<&reset>("Reset"),
                                ts::command("Slot", commandSlot)};
const ts::ServiceTable services{ts::service<&query>("Query"), ts::service<&borrow>("Borrow"),
                                ts::service<&notify>("Notify"), ts::service<&large>("Large"),
                                ts::service("Slot", serviceSlot)};
const ts::testing::CommandCatalogTable commandCatalog{ts::testing::group("Control", commands)};
const ts::testing::ServiceCatalogTable serviceCatalog{ts::testing::group("Control", services)};

// A factory result with deleted copy/move makes guaranteed direct construction
// observable independently of optimizer choices for a trivially copyable DTO.
struct Immobile {
	static inline unsigned constructions = 0, destructions = 0;
	const Immobile* self = this;

	Immobile() noexcept
	{
		++constructions;
	}

	Immobile(const Immobile&) = delete;
	Immobile(Immobile&&) = delete;

	~Immobile() noexcept
	{
		++destructions;
	}
};
} // namespace

int main()
{
	using Owned = ts::ServiceResult<Reply>;
	using Borrowed = ts::BorrowedServiceResult<Reply>;
	static_assert(std::is_trivially_copyable_v<ts::NativeCallResult<ts::CommandResult>>);
	static_assert(sizeof(ts::NativeCallResult<ts::CommandResult>) <= 4);
	static_assert(std::same_as<decltype(commands.callAs(0, Request{})),
	                           ts::NativeCallResult<ts::CommandResult>>);
	static_assert(
	    std::same_as<decltype(services.callAs<Owned>(0, Request{})), ts::NativeCallResult<Owned>>);
	auto command = commands.callAs(0, Request{1});
	CHECK(command.status() == ts::NativeCallStatus::Ok);
	CHECK(command.hasValue() && command.value() == ts::CommandResult::Executed);
	CHECK(commands.callAs(0, Request{0}).value() == ts::CommandResult::Busy);
	CHECK(commands.callAs(1).value() == ts::CommandResult::Accepted);
	const auto before = calls;
	CHECK(commands.callAs(0).status() == ts::NativeCallStatus::SignatureMismatch);
	CHECK(commands.callAs(1, Request{1}).status() == ts::NativeCallStatus::SignatureMismatch);
	CHECK(commands.callAs(0, Other{1}).status() == ts::NativeCallStatus::SignatureMismatch);
	CHECK(commands.callAs(-1, Request{1}).status() == ts::NativeCallStatus::NotFound);
	CHECK(commands.callAs(std::uint64_t{1} << 32, Request{1}).status() ==
	      ts::NativeCallStatus::NotFound);
	CHECK(calls == before);
	CHECK(commandCatalog.callAs(0, Request{3}).value() == ts::CommandResult::Executed);
	CHECK(commandCatalog.callAs(1).value() == ts::CommandResult::Accepted);
	CHECK(commandCatalog.callAs(0xffff, Request{}).status() == ts::NativeCallStatus::NotFound);
	CHECK(commandCatalog.callAs(0x10000, Request{}).status() == ts::NativeCallStatus::NotFound);
	CHECK(commandCatalog.callAs(std::uint64_t{1} << 32, Request{}).status() ==
	      ts::NativeCallStatus::NotFound);
	CHECK(commands.callAs(2, Request{}).value() == ts::CommandResult::Unavailable);
	commandSlot.bind(&configure);
	CHECK(commands.callAs(2, Request{1}).value() == ts::CommandResult::Executed);
	commandSlot.reset();
	CHECK(commands.callAs(2, Request{}).value() == ts::CommandResult::Unavailable);
	auto reply = services.callAs<Owned>(0, Request{29});
	CHECK(reply.status() == ts::NativeCallStatus::Ok && reply.value().hasValue());
	CHECK(reply.value().value().value == 29);
	auto refused = services.callAs<Owned>(0, Request{});
	CHECK(refused.hasValue() && !refused.value().hasValue());
	CHECK(refused.value().status() == ts::ServiceStatus::Busy);
	const auto serviceBefore = calls;
	CHECK(services.callAs<Owned>(0).status() == ts::NativeCallStatus::SignatureMismatch);
	CHECK(services.callAs<Borrowed>(0, Request{1}).status() ==
	      ts::NativeCallStatus::SignatureMismatch);
	CHECK(services.callAs<Owned>(1, Request{1}).status() ==
	      ts::NativeCallStatus::SignatureMismatch);
	CHECK(services.callAs<Owned>(0, Other{}).status() == ts::NativeCallStatus::SignatureMismatch);
	CHECK(services.callAs<Owned>(-1, Request{}).status() == ts::NativeCallStatus::NotFound);
	CHECK(services.callAs<Owned>(std::uint64_t{1} << 32, Request{}).status() ==
	      ts::NativeCallStatus::NotFound);
	CHECK(calls == serviceBefore);
	auto view = serviceCatalog.callAs<Borrowed>(1, Request{});
	CHECK(view.hasValue() && view.value().valueOrNull() == &borrowed);
	borrowed.value = 31;
	CHECK(view.value().value().value == 31);
	CHECK(services.callAs<ts::ServiceResult<void>>(2).value().hasValue());
	auto big = serviceCatalog.callAs<ts::ServiceResult<Big>>(3, Request{91});
	CHECK(big.hasValue() && big.value().value().values[0] == 91);
	CHECK(big.value().value().values[1023] == 0);
	CHECK(services.callAs<Owned>(4, Request{}).value().status() == ts::ServiceStatus::Unavailable);
	serviceSlot.bind(&query);
	CHECK(serviceCatalog.callAs<Owned>(4, Request{53}).value().value().value == 53);
	serviceSlot.reset();
	CHECK(serviceCatalog.callAs<Owned>(4, Request{}).value().status() ==
	      ts::ServiceStatus::Unavailable);
	CHECK(serviceCatalog.callAs<Owned>(0xffff, Request{}).status() ==
	      ts::NativeCallStatus::NotFound);
	CHECK(serviceCatalog.callAs<Owned>(0x10000, Request{}).status() ==
	      ts::NativeCallStatus::NotFound);
	CHECK(serviceCatalog.callAs<Owned>(std::uint64_t{1} << 32, Request{}).status() ==
	      ts::NativeCallStatus::NotFound);
	const ts::CommandTable<> emptyCommands{};
	const ts::ServiceTable<> emptyServices{};
	const ts::testing::CommandCatalogTable<> emptyCommandCatalog{};
	const ts::testing::ServiceCatalogTable<> emptyServiceCatalog{};
	CHECK(emptyCommands.callAs(0).status() == ts::NativeCallStatus::NotFound);
	CHECK(emptyServices.callAs<Owned>(0).status() == ts::NativeCallStatus::NotFound);
	CHECK(emptyCommandCatalog.callAs(0).status() == ts::NativeCallStatus::NotFound);
	CHECK(emptyServiceCatalog.callAs<Owned>(0).status() == ts::NativeCallStatus::NotFound);
	{
		auto final = ts::NativeCallResult<Immobile>::successFrom([]() noexcept {
			return Immobile{};
		});
		CHECK(final.hasValue() && final.value().self == &final.value());
		CHECK(Immobile::constructions == 1 && Immobile::destructions == 0);
	}
	CHECK(Immobile::destructions == 1);
	using Small = ts::NativeCallResult<Owned>;
	auto copy = reply;
	CHECK(copy.value().value().value == 29);
	copy = Small::failure(ts::NativeCallStatus::NotFound);
	CHECK(!copy.hasValue() && copy.valueOrNull() == nullptr);
	copy = reply;
	CHECK(copy.hasValue() && copy.value().value().value == 29);
	auto* sameCopy = &copy;
	copy = *sameCopy;
	CHECK(copy.hasValue());
	auto moved = std::move(copy);
	CHECK(moved.value().value().value == 29);
	auto* sameMoved = &moved;
	moved = std::move(*sameMoved);
	CHECK(moved.hasValue());
	std::printf("NativeCalls: %u checks passed\n", checks);
}

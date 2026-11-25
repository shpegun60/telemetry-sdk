/*
 * @file FlatCalls.cpp
 * @brief Independent flat native routing, ownership, final storage and lifetime checks.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// A native call must not allocate. The checks themselves use only fixed storage.
#if !defined(__arm__)
void* operator new(std::size_t)
{
	std::abort();
}

void* operator new[](std::size_t)
{
	std::abort();
}

void* operator new(std::size_t, std::align_val_t)
{
	std::abort();
}

void* operator new[](std::size_t, std::align_val_t)
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

void operator delete(void*, std::align_val_t) noexcept
{
	std::abort();
}

void operator delete[](void*, std::align_val_t) noexcept
{
	std::abort();
}

void operator delete(void*, std::size_t, std::align_val_t) noexcept
{
	std::abort();
}

void operator delete[](void*, std::size_t, std::align_val_t) noexcept
{
	std::abort();
}
#endif

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

struct OtherRequest {
	std::uint32_t value;
};

struct Reply {
	std::uint32_t value;
};

struct OtherReply {
	std::uint32_t value;
};

unsigned calls = 0;
ts::ServiceStatus serviceStatus = ts::ServiceStatus::Ok;
ts::CommandResult commandStatus = ts::CommandResult::Executed;
Reply backing{73};

ts::CommandResult command(const Request& request) noexcept
{
	++calls;
	return static_cast<ts::CommandResult>(request.value);
}

ts::CommandResult commandNoRequest() noexcept
{
	++calls;
	return commandStatus;
}

ts::CommandResult commandVoid() noexcept
{
	++calls;
	return ts::CommandResult::Executed;
}

ts::ServiceResult<Reply> own(const Request& request) noexcept
{
	++calls;
	if (request.value != 0)
		return ts::ServiceResult<Reply>::failure(static_cast<ts::ServiceStatus>(request.value));
	return ts::ServiceResult<Reply>::successFrom([]() noexcept {
		return Reply{73};
	});
}

ts::ServiceResult<Reply> ownNoRequest() noexcept
{
	return own(Request{static_cast<std::uint32_t>(serviceStatus)});
}

Reply plain(Request request) noexcept
{
	++calls;
	return {request.value};
}

Reply plainNoRequest() noexcept
{
	++calls;
	return {73};
}

ts::ServiceResult<void> voidResponse(const Request& request) noexcept
{
	++calls;
	if (request.value != 0)
		return ts::ServiceResult<void>::failure(static_cast<ts::ServiceStatus>(request.value));
	return ts::ServiceResult<void>::success();
}

ts::ServiceResult<void> voidNoRequest() noexcept
{
	return voidResponse(Request{static_cast<std::uint32_t>(serviceStatus)});
}

void plainVoid() noexcept
{
	++calls;
}

ts::BorrowedServiceResult<Reply> borrow(const Request& request) noexcept
{
	++calls;
	if (request.value != 0)
		return ts::BorrowedServiceResult<Reply>::failure(
		    static_cast<ts::ServiceStatus>(request.value));
	return ts::BorrowedServiceResult<Reply>::success(backing);
}

ts::BorrowedServiceResult<Reply> borrowNoRequest() noexcept
{
	return borrow(Request{static_cast<std::uint32_t>(serviceStatus)});
}

const Reply& plainBorrow() noexcept
{
	++calls;
	return backing;
}

// Wire endpoints require trivial aggregate payloads. A scalar address initialized
// from this records final construction without adding a pointer to the wire type.
// Any extra copy/move preserves the old address and is therefore detectable.
template<std::size_t Bytes>
struct Large {
	static inline unsigned constructions = 0;
	std::uintptr_t address = construct(this);
	std::array<std::uint8_t, Bytes - sizeof(std::uintptr_t)> bytes{};

	static std::uintptr_t construct(const Large* value) noexcept
	{
		++constructions;
		return reinterpret_cast<std::uintptr_t>(value);
	}
};

template<std::size_t Bytes>
ts::ServiceResult<Large<Bytes>> large(const Request& request) noexcept
{
	++calls;
	return ts::ServiceResult<Large<Bytes>>::successFrom([&]() noexcept {
		return Large<Bytes>{.bytes = {static_cast<std::uint8_t>(request.value)}};
	});
}

struct Owner {
	ts::CommandResult command(const Request& request) const noexcept
	{
		return ::command(request);
	}

	ts::CommandResult commandNoRequest() const noexcept
	{
		return ::commandNoRequest();
	}

	ts::ServiceResult<Reply> own(const Request& request) const noexcept
	{
		return ::own(request);
	}

	ts::BorrowedServiceResult<Reply> borrow(const Request& request) const noexcept
	{
		return ::borrow(request);
	}

	ts::ServiceResult<void> voidResponse(const Request& request) const noexcept
	{
		return ::voidResponse(request);
	}
};

ts::FunctionSlot<ts::CommandResult(const Request&) noexcept> commandSlot;
ts::FunctionSlot<ts::CommandResult() noexcept> commandNoRequestSlot;
ts::FunctionSlot<ts::ServiceResult<Reply>(const Request&) noexcept> ownSlot;
ts::FunctionSlot<ts::BorrowedServiceResult<Reply>(const Request&) noexcept> borrowSlot;
ts::FunctionSlot<ts::ServiceResult<void>(const Request&) noexcept> voidSlot;
ts::OwnerSlot<const Owner> ownerSlot;

const ts::CommandTable commands{ts::command<&command>("Command"),
                                ts::command<&commandNoRequest>("NoRequest"),
                                ts::command<&commandVoid>("Void"),
                                ts::command("Function", commandSlot),
                                ts::command("NoRequestFunction", commandNoRequestSlot),
                                ts::command<&Owner::command>("Owner", ownerSlot),
                                ts::command<&Owner::commandNoRequest>("NoRequestOwner", ownerSlot)};

const ts::ServiceTable services{ts::service<&own>("Own"),
                                ts::service<&ownNoRequest>("OwnNoRequest"),
                                ts::service<&plain>("Plain"),
                                ts::service<&plainNoRequest>("PlainNoRequest"),
                                ts::service<&voidResponse>("Void"),
                                ts::service<&voidNoRequest>("VoidNoRequest"),
                                ts::service<&plainVoid>("PlainVoid"),
                                ts::service<&borrow>("Borrow"),
                                ts::service<&borrowNoRequest>("BorrowNoRequest"),
                                ts::service<&plainBorrow>("PlainBorrow"),
                                ts::service("OwnFunction", ownSlot),
                                ts::service("BorrowFunction", borrowSlot),
                                ts::service("VoidFunction", voidSlot),
                                ts::service<&Owner::own>("OwnOwner", ownerSlot),
                                ts::service<&Owner::borrow>("BorrowOwner", ownerSlot),
                                ts::service<&Owner::voidResponse>("VoidOwner", ownerSlot),
                                ts::service<&large<1024>>("Large1024"),
                                ts::service<&large<4096>>("Large4096")};

const ts::CommandTable<> emptyCommands{};
const ts::ServiceTable<> emptyServices{};
const ts::CommandTable extraCommands{ts::command<&commandNoRequest>("Extra")};
const ts::ServiceTable extraServices{ts::service<&plainNoRequest>("Extra")};
const ts::testing::CommandCatalogTable commandCatalog{ts::testing::group("Empty", emptyCommands),
                                                      ts::testing::group("Control", commands),
                                                      ts::testing::group("Extra", extraCommands)};
const ts::testing::ServiceCatalogTable serviceCatalog{ts::testing::group("Empty", emptyServices),
                                                      ts::testing::group("Control", services),
                                                      ts::testing::group("Extra", extraServices)};

constexpr std::uint32_t packed(std::uint32_t entry) noexcept
{
	return 0x10000u | entry;
}

constexpr std::array serviceStatuses{
    ts::ServiceCallStatus::Ok, ts::ServiceCallStatus::InvalidArgument,
    ts::ServiceCallStatus::Unavailable, ts::ServiceCallStatus::Busy, ts::ServiceCallStatus::Failed};
constexpr std::array commandStatuses{ts::CommandCallStatus::Executed,
                                     ts::CommandCallStatus::Accepted,
                                     ts::CommandCallStatus::NotFound,
                                     ts::CommandCallStatus::Unavailable,
                                     ts::CommandCallStatus::ArgumentCountMismatch,
                                     ts::CommandCallStatus::InvalidValue,
                                     ts::CommandCallStatus::Busy,
                                     ts::CommandCallStatus::Failed};

template<class Result>
void checkReply(Result& result, ts::ServiceCallStatus expected)
{
	CHECK(result.status() == expected);
	CHECK(result.hasValue() == (expected == ts::ServiceCallStatus::Ok));
	CHECK(static_cast<bool>(result) == result.hasValue());
	CHECK((result.valueOrNull() != nullptr) == result.hasValue());
	if (result) {
		CHECK(result.value().value == 73);
		CHECK(result.valueOrNull() == std::addressof(result.value()));
	}
}

template<class Result>
void checkVoid(Result& result, ts::ServiceCallStatus expected)
{
	CHECK(result.status() == expected);
	CHECK(result.hasValue() == (expected == ts::ServiceCallStatus::Ok));
	CHECK(static_cast<bool>(result) == result.hasValue());
	if (result) {
		result.value();
		CHECK(true);
	}
}

void checkStatuses()
{
	for (std::uint32_t n = 0; n < serviceStatuses.size(); ++n) {
		const Request request{n};
		serviceStatus = static_cast<ts::ServiceStatus>(n);
		auto local = services.callAs<Reply>(0, request);
		auto global = serviceCatalog.callAs<Reply>(packed(0), request);
		auto noRequest = services.callAs<Reply>(1);
		auto noRequestGlobal = serviceCatalog.callAs<Reply>(packed(1));
		auto borrowed = services.callBorrowed<Reply>(7, request);
		auto borrowedGlobal = serviceCatalog.callBorrowed<Reply>(packed(7), request);
		auto borrowedNoRequest = services.callBorrowed<Reply>(8);
		auto borrowedNoRequestGlobal = serviceCatalog.callBorrowed<Reply>(packed(8));
		auto nothing = services.callAs<void>(4, request);
		auto nothingGlobal = serviceCatalog.callAs<void>(packed(4), request);
		auto nothingNoRequest = services.callAs<void>(5);
		auto nothingNoRequestGlobal = serviceCatalog.callAs<void>(packed(5));
		checkReply(local, serviceStatuses[n]);
		checkReply(global, serviceStatuses[n]);
		checkReply(noRequest, serviceStatuses[n]);
		checkReply(noRequestGlobal, serviceStatuses[n]);
		checkReply(borrowed, serviceStatuses[n]);
		checkReply(borrowedGlobal, serviceStatuses[n]);
		checkReply(borrowedNoRequest, serviceStatuses[n]);
		checkReply(borrowedNoRequestGlobal, serviceStatuses[n]);
		checkVoid(nothing, serviceStatuses[n]);
		checkVoid(nothingGlobal, serviceStatuses[n]);
		checkVoid(nothingNoRequest, serviceStatuses[n]);
		checkVoid(nothingNoRequestGlobal, serviceStatuses[n]);
		if (n == 0) {
			CHECK(borrowed.valueOrNull() == &backing);
			CHECK(borrowedGlobal.valueOrNull() == &backing);
			CHECK(borrowedNoRequest.valueOrNull() == &backing);
			CHECK(borrowedNoRequestGlobal.valueOrNull() == &backing);
		}
	}
	for (std::uint32_t n = 0; n < commandStatuses.size(); ++n) {
		commandStatus = static_cast<ts::CommandResult>(n);
		CHECK(commands.call(0, Request{n}) == commandStatuses[n]);
		CHECK(commandCatalog.call(packed(0), Request{n}) == commandStatuses[n]);
		CHECK(commands.call(1) == commandStatuses[n]);
		CHECK(commandCatalog.call(packed(1)) == commandStatuses[n]);
	}
	serviceStatus = ts::ServiceStatus::Ok;
	commandStatus = ts::CommandResult::Executed;
	CHECK(commands.call(2) == ts::CommandCallStatus::Executed);
	auto plainResult = services.callAs<Reply>(2, Request{73});
	auto plainNoRequestResult = serviceCatalog.callAs<Reply>(packed(3));
	auto plainVoidResult = services.callAs<void>(6);
	auto plainBorrowResult = serviceCatalog.callBorrowed<Reply>(packed(9));
	checkReply(plainResult, ts::ServiceCallStatus::Ok);
	checkReply(plainNoRequestResult, ts::ServiceCallStatus::Ok);
	checkVoid(plainVoidResult, ts::ServiceCallStatus::Ok);
	CHECK(plainBorrowResult.valueOrNull() == &backing);
	backing.value = 91;
	CHECK(plainBorrowResult.value().value == 91);
	backing.value = 73;
}

void checkRouting()
{
	const unsigned before = calls;
	CHECK(commands.call(0) == ts::CommandCallStatus::SignatureMismatch);
	CHECK(commands.call(1, Request{}) == ts::CommandCallStatus::SignatureMismatch);
	CHECK(commandCatalog.call(packed(0), OtherRequest{}) ==
	      ts::CommandCallStatus::SignatureMismatch);
	CHECK(serviceCatalog.callAs<OtherReply>(packed(0), Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callAs<Reply>(0, OtherRequest{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callAs<Reply>(0).status() == ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callAs<Reply>(1, Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callBorrowed<Reply>(0, Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(serviceCatalog.callAs<Reply>(packed(7), Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callBorrowed<OtherReply>(7, Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callBorrowed<Reply>(7).status() == ts::ServiceCallStatus::SignatureMismatch);
	CHECK(serviceCatalog.callBorrowed<Reply>(packed(8), Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callAs<void>(0, Request{}).status() == ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callAs<Reply>(4, Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	const Request request{};
	CHECK(services.callAs<Reply>(0, &request).status() == ts::ServiceCallStatus::SignatureMismatch);
	CHECK(services.callBorrowed<Reply>(7, &request).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(commands.call(0, &request) == ts::CommandCallStatus::SignatureMismatch);
	CHECK(services.callAs<Reply>(0, std::uint32_t{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(calls == before);

	for (const std::int64_t id :
	     std::array<std::int64_t, 3>{-1, -65536, std::numeric_limits<std::int64_t>::min()}) {
		CHECK(commands.call(id, Request{}) == ts::CommandCallStatus::NotFound);
		CHECK(commandCatalog.call(id, Request{}) == ts::CommandCallStatus::NotFound);
		CHECK(services.callAs<Reply>(id, Request{}).status() == ts::ServiceCallStatus::NotFound);
		CHECK(serviceCatalog.callAs<Reply>(id, Request{}).status() ==
		      ts::ServiceCallStatus::NotFound);
		CHECK(services.callBorrowed<Reply>(id, Request{}).status() ==
		      ts::ServiceCallStatus::NotFound);
		CHECK(serviceCatalog.callBorrowed<Reply>(id, Request{}).status() ==
		      ts::ServiceCallStatus::NotFound);
	}
	for (const std::uint64_t id : {std::uint64_t{1} << 32, (std::uint64_t{1} << 32) | packed(0),
	                               std::numeric_limits<std::uint64_t>::max()}) {
		CHECK(commands.call(id, Request{}) == ts::CommandCallStatus::NotFound);
		CHECK(commandCatalog.call(id, Request{}) == ts::CommandCallStatus::NotFound);
		CHECK(services.callAs<Reply>(id, Request{}).status() == ts::ServiceCallStatus::NotFound);
		CHECK(serviceCatalog.callAs<Reply>(id, Request{}).status() ==
		      ts::ServiceCallStatus::NotFound);
		CHECK(services.callBorrowed<Reply>(id, Request{}).status() ==
		      ts::ServiceCallStatus::NotFound);
		CHECK(serviceCatalog.callBorrowed<Reply>(id, Request{}).status() ==
		      ts::ServiceCallStatus::NotFound);
	}
	for (const unsigned id : {0xffffu, 0x10000u}) {
		CHECK(commands.call(id) == ts::CommandCallStatus::NotFound);
		CHECK(services.callAs<Reply>(id).status() == ts::ServiceCallStatus::NotFound);
		CHECK(services.callBorrowed<Reply>(id).status() == ts::ServiceCallStatus::NotFound);
	}
	for (const unsigned id : {0u, 0xffffu, 0x1ffffu, 0x30000u, 0xffffffffu}) {
		CHECK(commandCatalog.call(id) == ts::CommandCallStatus::NotFound);
		CHECK(serviceCatalog.callAs<Reply>(id).status() == ts::ServiceCallStatus::NotFound);
		CHECK(serviceCatalog.callBorrowed<Reply>(id).status() == ts::ServiceCallStatus::NotFound);
	}
	CHECK(calls == before);
	CHECK(commandCatalog.call(0x20000) == ts::CommandCallStatus::Executed);
	auto otherGroup = serviceCatalog.callAs<Reply>(0x20000);
	checkReply(otherGroup, ts::ServiceCallStatus::Ok);

	const ts::testing::CommandCatalogTable<> noCommandGroups{};
	const ts::testing::ServiceCatalogTable<> noServiceGroups{};
	CHECK(emptyCommands.call(0) == ts::CommandCallStatus::NotFound);
	CHECK(emptyCommands.call(0, Request{}) == ts::CommandCallStatus::NotFound);
	CHECK(noCommandGroups.call(0) == ts::CommandCallStatus::NotFound);
	CHECK(noCommandGroups.call(0, Request{}) == ts::CommandCallStatus::NotFound);
	CHECK(emptyServices.callAs<Reply>(0).status() == ts::ServiceCallStatus::NotFound);
	CHECK(emptyServices.callAs<void>(0, Request{}).status() == ts::ServiceCallStatus::NotFound);
	CHECK(emptyServices.callBorrowed<Reply>(0).status() == ts::ServiceCallStatus::NotFound);
	CHECK(emptyServices.callBorrowed<Reply>(0, Request{}).status() ==
	      ts::ServiceCallStatus::NotFound);
	CHECK(noServiceGroups.callAs<Reply>(0).status() == ts::ServiceCallStatus::NotFound);
	CHECK(noServiceGroups.callAs<void>(0, Request{}).status() == ts::ServiceCallStatus::NotFound);
	CHECK(noServiceGroups.callBorrowed<Reply>(0).status() == ts::ServiceCallStatus::NotFound);
	CHECK(noServiceGroups.callBorrowed<Reply>(0, Request{}).status() ==
	      ts::ServiceCallStatus::NotFound);
}

void checkSlots()
{
	const unsigned before = calls;
	for (unsigned row : {3u, 5u}) {
		CHECK(commands.call(row, Request{}) == ts::CommandCallStatus::Unavailable);
		CHECK(commandCatalog.call(packed(row), Request{}) == ts::CommandCallStatus::Unavailable);
		CHECK(commands.call(row, OtherRequest{}) == ts::CommandCallStatus::SignatureMismatch);
	}
	for (unsigned row : {4u, 6u}) {
		CHECK(commands.call(row) == ts::CommandCallStatus::Unavailable);
		CHECK(commandCatalog.call(packed(row)) == ts::CommandCallStatus::Unavailable);
	}
	for (unsigned row : {10u, 13u}) {
		auto result = services.callAs<Reply>(row, Request{});
		checkReply(result, ts::ServiceCallStatus::Unavailable);
		CHECK(serviceCatalog.callAs<Reply>(packed(row), Request{}).status() ==
		      ts::ServiceCallStatus::Unavailable);
		CHECK(services.callBorrowed<Reply>(row, Request{}).status() ==
		      ts::ServiceCallStatus::SignatureMismatch);
	}
	for (unsigned row : {11u, 14u}) {
		auto result = services.callBorrowed<Reply>(row, Request{});
		checkReply(result, ts::ServiceCallStatus::Unavailable);
		CHECK(serviceCatalog.callBorrowed<Reply>(packed(row), Request{}).status() ==
		      ts::ServiceCallStatus::Unavailable);
		CHECK(services.callAs<Reply>(row, Request{}).status() ==
		      ts::ServiceCallStatus::SignatureMismatch);
	}
	for (unsigned row : {12u, 15u}) {
		auto result = services.callAs<void>(row, Request{});
		checkVoid(result, ts::ServiceCallStatus::Unavailable);
		CHECK(serviceCatalog.callAs<void>(packed(row), Request{}).status() ==
		      ts::ServiceCallStatus::Unavailable);
	}
	CHECK(calls == before);
	const Owner owner{};
	commandSlot.bind(&command);
	commandNoRequestSlot.bind(&commandNoRequest);
	ownSlot.bind(&own);
	borrowSlot.bind(&borrow);
	voidSlot.bind(&voidResponse);
	ownerSlot.bind(owner);
	for (std::uint32_t n = 0; n < serviceStatuses.size(); ++n) {
		for (unsigned row : {10u, 13u}) {
			auto result = services.callAs<Reply>(row, Request{n});
			checkReply(result, serviceStatuses[n]);
		}
		for (unsigned row : {11u, 14u}) {
			auto result = serviceCatalog.callBorrowed<Reply>(packed(row), Request{n});
			checkReply(result, serviceStatuses[n]);
			if (result)
				CHECK(result.valueOrNull() == &backing);
		}
		for (unsigned row : {12u, 15u}) {
			auto result = serviceCatalog.callAs<void>(packed(row), Request{n});
			checkVoid(result, serviceStatuses[n]);
		}
	}
	for (std::uint32_t n = 0; n < commandStatuses.size(); ++n) {
		commandStatus = static_cast<ts::CommandResult>(n);
		CHECK(commands.call(3, Request{n}) == commandStatuses[n]);
		CHECK(commandCatalog.call(packed(5), Request{n}) == commandStatuses[n]);
		CHECK(commands.call(4) == commandStatuses[n]);
		CHECK(commandCatalog.call(packed(6)) == commandStatuses[n]);
	}
	commandSlot.reset();
	commandNoRequestSlot.reset();
	ownSlot.reset();
	borrowSlot.reset();
	voidSlot.reset();
	ownerSlot.reset();
	commandStatus = ts::CommandResult::Executed;
	CHECK(commands.call(3, Request{}) == ts::CommandCallStatus::Unavailable);
	CHECK(commands.call(6) == ts::CommandCallStatus::Unavailable);
	CHECK(services.callAs<Reply>(13, Request{}).status() == ts::ServiceCallStatus::Unavailable);
	CHECK(services.callBorrowed<Reply>(11, Request{}).status() ==
	      ts::ServiceCallStatus::Unavailable);
	CHECK(services.callAs<void>(12, Request{}).status() == ts::ServiceCallStatus::Unavailable);
}

template<std::size_t Bytes>
void checkLarge(unsigned row)
{
	using Response = Large<Bytes>;
	using Result = ts::ServiceCallResult<Response>;
	static_assert(sizeof(Response) == Bytes);
	static_assert(std::is_trivially_copyable_v<Response>);
	alignas(Result) std::array<std::byte, sizeof(Result)> storage{};
	Response::constructions = 0;
	auto* result = ::new (storage.data()) Result(services.callAs<Response>(row, Request{91}));
	CHECK(result->status() == ts::ServiceCallStatus::Ok);
	CHECK(result->value().address == reinterpret_cast<std::uintptr_t>(&result->value()));
	CHECK(result->value().bytes.front() == 91 && result->value().bytes.back() == 0);
	CHECK(Response::constructions == 1);
	std::destroy_at(result);
	Response::constructions = 0;
	result =
	    ::new (storage.data()) Result(serviceCatalog.callAs<Response>(packed(row), Request{53}));
	CHECK(result->value().address == reinterpret_cast<std::uintptr_t>(&result->value()));
	CHECK(result->value().bytes.front() == 53 && result->value().bytes.back() == 0);
	CHECK(Response::constructions == 1);
	std::destroy_at(result);
	Response::constructions = 0;
	auto mismatch = services.callAs<Response>(row, OtherRequest{});
	CHECK(mismatch.status() == ts::ServiceCallStatus::SignatureMismatch && !mismatch);
	CHECK(Response::constructions == 0);
	auto absent = serviceCatalog.callAs<Response>(std::uint64_t{1} << 32, Request{});
	CHECK(absent.status() == ts::ServiceCallStatus::NotFound && absent.valueOrNull() == nullptr);
	CHECK(Response::constructions == 0);
}

// Nontrivial response objects cannot be wire endpoints. Exercise the result
// facade itself with explicit factories to audit copy/move/destruction separately.
struct Tracked {
	static inline unsigned created = 0, destroyed = 0, copies = 0, moves = 0, assigns = 0;
	const Tracked* address = this;
	unsigned value = 73;
	std::array<std::uint8_t, 4096 - sizeof(Tracked*) - sizeof(unsigned)> bytes{};

	Tracked() noexcept
	{
		++created;
	}

	Tracked(const Tracked& other) noexcept : value(other.value)
	{
		++created;
		++copies;
	}

	Tracked(Tracked&& other) noexcept : value(other.value)
	{
		++created;
		++moves;
	}

	Tracked& operator=(const Tracked& other) noexcept
	{
		value = other.value;
		++assigns;
		return *this;
	}

	Tracked& operator=(Tracked&& other) noexcept
	{
		value = other.value;
		++assigns;
		return *this;
	}

	~Tracked() noexcept
	{
		++destroyed;
	}
};

struct Immobile {
	static inline unsigned created = 0, destroyed = 0;
	const Immobile* address = this;
	std::array<std::uint8_t, 4096 - sizeof(Immobile*)> bytes{};

	Immobile() noexcept
	{
		++created;
	}

	Immobile(const Immobile&) = delete;
	Immobile(Immobile&&) = delete;
	Immobile& operator=(const Immobile&) = delete;
	Immobile& operator=(Immobile&&) = delete;

	~Immobile() noexcept
	{
		++destroyed;
	}
};

template<class Value>
auto factoryResult()
{
	return ts::ServiceCallResult<Value>::fromNative([]() noexcept {
		return ts::NativeCallResult<ts::ServiceResult<Value>>::successFrom([]() noexcept {
			return ts::ServiceResult<Value>::successFrom([]() noexcept {
				return Value{};
			});
		});
	});
}

template<class Value>
auto routeFailure()
{
	return ts::ServiceCallResult<Value>::fromNative([]() noexcept {
		return ts::NativeCallResult<ts::ServiceResult<Value>>::failure(
		    ts::NativeCallStatus::NotFound);
	});
}

template<class Value>
auto applicationFailure()
{
	return ts::ServiceCallResult<Value>::fromNative([]() noexcept {
		return ts::NativeCallResult<ts::ServiceResult<Value>>::successFrom([]() noexcept {
			return ts::ServiceResult<Value>::failure(ts::ServiceStatus::Busy);
		});
	});
}

void checkLifetime()
{
	static_assert(sizeof(Tracked) == 4096 && sizeof(Immobile) == 4096);
	{
		auto absent = routeFailure<Immobile>();
		auto refused = applicationFailure<Immobile>();
		CHECK(!absent && absent.valueOrNull() == nullptr);
		CHECK(!refused && refused.valueOrNull() == nullptr);
		CHECK(Immobile::created == 0 && Immobile::destroyed == 0);
		auto final = factoryResult<Immobile>();
		CHECK(final.value().address == &final.value());
		CHECK(Immobile::created == 1 && Immobile::destroyed == 0);
	}
	CHECK(Immobile::created == 1 && Immobile::destroyed == 1);
	{
		auto source = factoryResult<Tracked>();
		CHECK(source.value().address == &source.value());
		CHECK(Tracked::created == 1 && Tracked::copies == 0 && Tracked::moves == 0);
		auto copied = source;
		CHECK(copied.value().value == 73 && copied.value().address == &copied.value());
		CHECK(Tracked::copies == 1 && Tracked::moves == 0);
		auto moved = std::move(copied);
		CHECK(moved.value().value == 73 && moved.value().address == &moved.value());
		CHECK(Tracked::copies == 1 && Tracked::moves == 1);
		auto* alias = &moved;
		moved = *alias;
		moved = std::move(*alias);
		CHECK(Tracked::assigns == 0 && moved);
		moved = source;
		CHECK(Tracked::assigns == 1);
		auto absent = routeFailure<Tracked>();
		moved = absent;
		CHECK(!moved && moved.valueOrNull() == nullptr && Tracked::destroyed == 1);
		absent = source;
		CHECK(absent && absent.value().address == &absent.value());
		CHECK(Tracked::copies == 2);
		auto refused = applicationFailure<Tracked>();
		absent = refused;
		CHECK(!absent && absent.status() == ts::ServiceCallStatus::Busy);
		CHECK(Tracked::destroyed == 2);
		moved = factoryResult<Tracked>();
		CHECK(moved && moved.value().address == &moved.value());
		CHECK(Tracked::moves == 2);
		moved = applicationFailure<Tracked>();
		CHECK(!moved && moved.status() == ts::ServiceCallStatus::Busy);
	}
	CHECK(Tracked::created == Tracked::destroyed);
#if defined(__cpp_exceptions)
	// The test factory throws before creating any library result or payload.
	// Exception propagation belongs to fromNative's conditional noexcept contract.
	const unsigned createdBefore = Tracked::created, destroyedBefore = Tracked::destroyed;
	bool caught = false;
	try {
		(void)ts::ServiceCallResult<Tracked>::fromNative(
		    []() -> ts::NativeCallResult<ts::ServiceResult<Tracked>> {
			    throw 123u;
		    });
	} catch (unsigned marker) {
		caught = true;
		CHECK(marker == 123);
	}
	CHECK(caught);
	CHECK(Tracked::created == createdBefore);
	CHECK(Tracked::destroyed == destroyedBefore);
#endif
}

// Owning pointers depend on the named wrapper's lifetime. Borrowed pointers
// instead refer to application storage and remain valid after a wrapper expires.
template<class Result>
concept HasValuePointer = requires(Result&& result) { std::forward<Result>(result).valueOrNull(); };

void checkValuePointers()
{
	using Owned = ts::ServiceCallResult<Reply>;
	using Borrowed = ts::BorrowedServiceCallResult<Reply>;
	using Nothing = ts::ServiceCallResult<void>;
	static_assert(HasValuePointer<Owned&> && HasValuePointer<const Owned&>);
	static_assert(!HasValuePointer<Owned&&> && !HasValuePointer<const Owned&&>);
	static_assert(HasValuePointer<Borrowed&> && HasValuePointer<const Borrowed&>);
	static_assert(HasValuePointer<Borrowed&&> && HasValuePointer<const Borrowed&&>);
	static_assert(!HasValuePointer<Nothing&> && !HasValuePointer<const Nothing&>);
	static_assert(!HasValuePointer<Nothing&&> && !HasValuePointer<const Nothing&&>);

	auto local = services.callAs<Reply>(0, Request{});
	auto global = serviceCatalog.callAs<Reply>(packed(0), Request{});
	const auto constLocal = services.callAs<Reply>(0, Request{});
	const auto constGlobal = serviceCatalog.callAs<Reply>(packed(0), Request{});
	CHECK(local.valueOrNull() == &local.value());
	CHECK(global.valueOrNull() == &global.value());
	CHECK(constLocal.valueOrNull() == &constLocal.value());
	CHECK(constGlobal.valueOrNull() == &constGlobal.value());
	local.valueOrNull()->value = 91;
	CHECK(local.value().value == 91);
	CHECK(constGlobal.valueOrNull()->value == 73);

	// The four temporary wrappers are gone before any pointer is inspected.
	const Reply* borrowedLocal = services.callBorrowed<Reply>(7, Request{}).valueOrNull();
	const Reply* borrowedGlobal =
	    serviceCatalog.callBorrowed<Reply>(packed(7), Request{}).valueOrNull();
	const Reply* constBorrowedLocal = []() -> const Borrowed {
		return services.callBorrowed<Reply>(7, Request{});
	}()
	                                              .valueOrNull();
	const Reply* constBorrowedGlobal = []() -> const Borrowed {
		return serviceCatalog.callBorrowed<Reply>(packed(7), Request{});
	}()
	                                               .valueOrNull();
	for (const Reply* pointer :
	     {borrowedLocal, borrowedGlobal, constBorrowedLocal, constBorrowedGlobal}) {
		CHECK(pointer == &backing);
		CHECK(pointer->value == 73);
	}
	backing.value = 91;
	for (const Reply* pointer :
	     {borrowedLocal, borrowedGlobal, constBorrowedLocal, constBorrowedGlobal})
		CHECK(pointer->value == 91);
	backing.value = 73;

	// Moving the payload out uses value(), whose rvalue overload remains public.
	Reply movedLocal = services.callAs<Reply>(0, Request{}).value();
	Reply movedGlobal = std::move(global).value();
	CHECK(movedLocal.value == 73);
	CHECK(movedGlobal.value == 73);
}

void checkTypesAndCompatibility()
{
	using Owned = ts::ServiceCallResult<Reply>;
	using Borrowed = ts::BorrowedServiceCallResult<Reply>;
	static_assert(std::same_as<decltype(commands.call(0, Request{})), ts::CommandCallStatus>);
	static_assert(std::same_as<decltype(commandCatalog.call(0)), ts::CommandCallStatus>);
	static_assert(std::same_as<decltype(services.callAs<Reply>(0, Request{})), Owned>);
	static_assert(std::same_as<decltype(serviceCatalog.callAs<Reply>(0)), Owned>);
	static_assert(std::same_as<decltype(services.callBorrowed<Reply>(0, Request{})), Borrowed>);
	static_assert(std::same_as<decltype(serviceCatalog.callBorrowed<Reply>(0)), Borrowed>);
	static_assert(std::same_as<decltype(std::declval<Owned&>().value()), Reply&>);
	static_assert(std::same_as<decltype(std::declval<const Owned&>().value()), const Reply&>);
	static_assert(std::same_as<decltype(std::declval<Owned&&>().value()), Reply&&>);
	static_assert(std::same_as<decltype(std::declval<const Owned&&>().value()), const Reply&&>);
	static_assert(std::same_as<decltype(std::declval<Borrowed&>().value()), const Reply&>);
	static_assert(std::same_as<decltype(std::declval<const Borrowed&&>().value()), const Reply&>);
	static_assert(std::same_as<decltype(std::declval<Borrowed&>().valueOrNull()), const Reply*>);
	static_assert(std::same_as<decltype(std::declval<Owned&>().valueOrNull()), Reply*>);
	static_assert(std::same_as<decltype(std::declval<const Owned&>().valueOrNull()), const Reply*>);
	static_assert(
	    std::same_as<decltype(std::declval<const ts::ServiceCallResult<void>&>().value()), void>);
	static_assert(!std::is_convertible_v<Owned, bool> && !std::is_convertible_v<Borrowed, bool>);
	static_assert(noexcept(commands.call(0, Request{})) && noexcept(commandCatalog.call(0)));
	static_assert(noexcept(services.callAs<Reply>(0, Request{})) &&
	              noexcept(serviceCatalog.callAs<void>(0)));
	static_assert(noexcept(services.callBorrowed<Reply>(0, Request{})) &&
	              noexcept(serviceCatalog.callBorrowed<Reply>(0)));
	static_assert(noexcept(std::declval<Owned&>().value()) &&
	              noexcept(std::declval<Owned&>().valueOrNull()));
	static_assert(noexcept(std::declval<const Owned&>().status()) &&
	              noexcept(std::declval<const Owned&>().hasValue()));
	static_assert(noexcept(std::declval<const Borrowed&>().value()) &&
	              noexcept(std::declval<const Borrowed&>().valueOrNull()));
	static_assert(noexcept(std::declval<const ts::ServiceCallResult<void>&>().value()));
	static_assert(sizeof(ts::ServiceCallResult<void>) <= 4);
	static_assert(std::is_trivially_copyable_v<Borrowed>);
	static_assert(!std::is_convertible_v<Borrowed, Owned> &&
	              !std::is_convertible_v<Owned, Borrowed>);
	constexpr auto nothrowFactory = []() noexcept {
		return ts::NativeCallResult<ts::ServiceResult<Reply>>::failure(
		    ts::NativeCallStatus::NotFound);
	};
	constexpr auto throwingFactory = []() {
		return ts::NativeCallResult<ts::ServiceResult<Reply>>::failure(
		    ts::NativeCallStatus::NotFound);
	};
	constexpr auto borrowedFactory = []() noexcept {
		return ts::NativeCallResult<ts::BorrowedServiceResult<Reply>>::failure(
		    ts::NativeCallStatus::NotFound);
	};
	static_assert(noexcept(Owned::fromNative(nothrowFactory)));
	static_assert(!noexcept(Owned::fromNative(throwingFactory)));
	static_assert(noexcept(Borrowed::fromNative(borrowedFactory)));
	static_assert(sizeof(ts::CommandResult) == 1);
	static_assert(static_cast<unsigned>(ts::CommandResult::Executed) == 0);
	static_assert(static_cast<unsigned>(ts::CommandResult::Accepted) == 1);
	static_assert(static_cast<unsigned>(ts::CommandResult::NotFound) == 2);
	static_assert(static_cast<unsigned>(ts::CommandResult::Unavailable) == 3);
	static_assert(static_cast<unsigned>(ts::CommandResult::ArgumentCountMismatch) == 4);
	static_assert(static_cast<unsigned>(ts::CommandResult::InvalidValue) == 5);
	static_assert(static_cast<unsigned>(ts::CommandResult::Busy) == 6);
	static_assert(static_cast<unsigned>(ts::CommandResult::Failed) == 7);
	static_assert(std::same_as<decltype(services.callAs<ts::ServiceResult<Reply>>(0, Request{})),
	                           ts::NativeCallResult<ts::ServiceResult<Reply>>>);
	static_assert(std::same_as<decltype(serviceCatalog.callAs<ts::BorrowedServiceResult<Reply>>(0)),
	                           ts::NativeCallResult<ts::BorrowedServiceResult<Reply>>>);
	CHECK(commands.call<0>(Request{}) == ts::CommandResult::Executed);
	CHECK(ts::testing::CatalogAccess::call<0x10000>(commandCatalog, Request{}) ==
	      ts::CommandResult::Executed);
	CHECK(services.call<0>(Request{}).value().value == 73);
	CHECK(ts::testing::CatalogAccess::call<0x10000>(serviceCatalog, Request{}).value().value == 73);
	auto oldOwned = services.callAs<ts::ServiceResult<Reply>>(0, Request{3});
	CHECK(oldOwned.hasValue() && !oldOwned.value().hasValue());
	CHECK(oldOwned.status() == ts::NativeCallStatus::Ok &&
	      oldOwned.value().status() == ts::ServiceStatus::Busy);
	auto oldBorrowed =
	    serviceCatalog.callAs<ts::BorrowedServiceResult<Reply>>(packed(7), Request{});
	CHECK(oldBorrowed.value().valueOrNull() == &backing);
	CHECK(commands.callAs(0, Request{6}).value() == ts::CommandResult::Busy);
	CHECK(commandCatalog.callAs(packed(0), Request{7}).value() == ts::CommandResult::Failed);
	const Request constRequest{};
	CHECK(services.callAs<Reply>(0, constRequest).status() == ts::ServiceCallStatus::Ok);
	CHECK(services.callBorrowed<Reply>(7, constRequest).valueOrNull() == &backing);
	CHECK(commands.call(0, constRequest) == ts::CommandCallStatus::Executed);
	enum class Local : std::int64_t {
		Own = 0,
		Borrow = 7,
		Negative = -1
	};
	enum class WideLocal : std::uint64_t {
		TooWide = std::uint64_t{1} << 32
	};
	CHECK(commands.call(Local::Own, constRequest) == ts::CommandCallStatus::Executed);
	CHECK(services.callAs<Reply>(Local::Own, constRequest).hasValue());
	CHECK(services.callBorrowed<Reply>(Local::Borrow, constRequest).valueOrNull() == &backing);
	CHECK(commands.call(Local::Negative, constRequest) == ts::CommandCallStatus::NotFound);
	CHECK(services.callAs<Reply>(Local::Negative, constRequest).status() ==
	      ts::ServiceCallStatus::NotFound);
	CHECK(services.callBorrowed<Reply>(Local::Negative, constRequest).status() ==
	      ts::ServiceCallStatus::NotFound);
	CHECK(commands.call(WideLocal::TooWide, constRequest) == ts::CommandCallStatus::NotFound);
	CHECK(services.callAs<Reply>(WideLocal::TooWide, constRequest).status() ==
	      ts::ServiceCallStatus::NotFound);
	CHECK(services.callBorrowed<Reply>(WideLocal::TooWide, constRequest).status() ==
	      ts::ServiceCallStatus::NotFound);
	for (unsigned raw : {8u, 255u}) {
		CHECK(commands.call(0, Request{raw}) == ts::CommandCallStatus::Failed);
		CHECK(commandCatalog.call(packed(0), Request{raw}) == ts::CommandCallStatus::Failed);
		CHECK(static_cast<unsigned>(commands.callAs(0, Request{raw}).value()) == raw);
		CHECK(static_cast<unsigned>(commandCatalog.callAs(packed(0), Request{raw}).value()) == raw);
	}
}
} // namespace

int main()
{
	checkTypesAndCompatibility();
	checkStatuses();
	checkRouting();
	checkSlots();
	checkLarge<1024>(16);
	checkLarge<4096>(17);
	checkLifetime();
	checkValuePointers();
	std::printf("FlatCalls: %u checks passed\n", checks);
}

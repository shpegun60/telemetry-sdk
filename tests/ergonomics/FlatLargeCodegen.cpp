/*
 * @file FlatLargeCodegen.cpp
 * @brief Final caller placement and borrowed Service paths for 1 and 4 KiB replies.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <new>
#include <type_traits>

#ifndef BIG_BYTES
#define BIG_BYTES 4096
#endif

namespace flat_large {
namespace ts = telemetry;

struct Request {
	std::uint32_t value;
};

struct Big {
	std::array<std::uint32_t, BIG_BYTES / sizeof(std::uint32_t)> values;
};

static_assert(BIG_BYTES == 1024 || BIG_BYTES == 4096);
static_assert(sizeof(Big) == BIG_BYTES);
inline Big backing{};
std::uint32_t ownedCalls = 0, borrowedCalls = 0;

ts::ServiceResult<Big> owned(const Request& request) noexcept
{
	++ownedCalls;
	return ts::ServiceResult<Big>::successFrom([&]() noexcept -> Big {
		return Big{{request.value}};
	});
}

const Big& borrowed(const Request&) noexcept
{
	++borrowedCalls;
	return backing;
}

inline constexpr ts::ServiceTable ownedTable{ts::service<&owned>("Owned")};
inline constexpr ts::ServiceTable borrowedTable{ts::service<&borrowed>("Borrowed")};
inline constexpr ts::testing::ServiceCatalogTable ownedCatalog{
    ts::testing::group("Buffers", ownedTable)};
inline constexpr ts::testing::ServiceCatalogTable borrowedCatalog{
    ts::testing::group("Buffers", borrowedTable)};

struct OwnedVisitor {
	void* output;
	const Request& request;

	template<class Definition>
	void operator()(const Definition& definition)
	{
		::new (output) ts::ServiceResult<Big>(definition.call(request));
	}
};

struct BorrowedVisitor {
	const Request& request;
	const Big* value = nullptr;

	template<class Definition>
	void operator()(const Definition& definition)
	{
		value = definition.call(request).valueOrNull();
	}
};

using NativeOwned = ts::NativeCallResult<ts::ServiceResult<Big>>;
using FlatOwned = ts::ServiceCallResult<Big>;
} // namespace flat_large

extern "C" void flat_manual_large_local(void* output, std::uint32_t id,
                                        const flat_large::Request& request)
{
	flat_large::OwnedVisitor visitor{output, request};
	(void)flat_large::ownedTable.visit(id, visitor);
}

extern "C" void flat_manual_large_global(void* output, std::uint32_t id,
                                         const flat_large::Request& request)
{
	flat_large::OwnedVisitor visitor{output, request};
	(void)flat_large::ownedCatalog.visit(id, visitor);
}

extern "C" void flat_native_large_local(void* output, std::uint32_t id,
                                        const flat_large::Request& request)
{
	::new (output) flat_large::NativeOwned(
	    flat_large::ownedTable.callAs<telemetry::ServiceResult<flat_large::Big>>(id, request));
}

extern "C" void flat_native_large_global(void* output, std::uint32_t id,
                                         const flat_large::Request& request)
{
	::new (output) flat_large::NativeOwned(
	    flat_large::ownedCatalog.callAs<telemetry::ServiceResult<flat_large::Big>>(id, request));
}

extern "C" void flat_api_large_local(void* output, std::uint32_t id,
                                     const flat_large::Request& request)
{
	::new (output)
	    flat_large::FlatOwned(flat_large::ownedTable.callAs<flat_large::Big>(id, request));
}

extern "C" void flat_api_large_global(void* output, std::uint32_t id,
                                      const flat_large::Request& request)
{
	::new (output)
	    flat_large::FlatOwned(flat_large::ownedCatalog.callAs<flat_large::Big>(id, request));
}

extern "C" const flat_large::Big* flat_manual_borrowed_local(std::uint32_t id,
                                                             const flat_large::Request& request)
{
	flat_large::BorrowedVisitor visitor{request};
	(void)flat_large::borrowedTable.visit(id, visitor);
	return visitor.value;
}

extern "C" const flat_large::Big* flat_manual_borrowed_global(std::uint32_t id,
                                                              const flat_large::Request& request)
{
	flat_large::BorrowedVisitor visitor{request};
	(void)flat_large::borrowedCatalog.visit(id, visitor);
	return visitor.value;
}

extern "C" const flat_large::Big* flat_native_borrowed_local(std::uint32_t id,
                                                             const flat_large::Request& request)
{
	auto result =
	    flat_large::borrowedTable.callAs<telemetry::BorrowedServiceResult<flat_large::Big>>(
	        id, request);
	return result.hasValue() ? result.value().valueOrNull() : nullptr;
}

extern "C" const flat_large::Big* flat_native_borrowed_global(std::uint32_t id,
                                                              const flat_large::Request& request)
{
	auto result =
	    flat_large::borrowedCatalog.callAs<telemetry::BorrowedServiceResult<flat_large::Big>>(
	        id, request);
	return result.hasValue() ? result.value().valueOrNull() : nullptr;
}

extern "C" const flat_large::Big* flat_api_borrowed_local(std::uint32_t id,
                                                          const flat_large::Request& request)
{
	return flat_large::borrowedTable.callBorrowed<flat_large::Big>(id, request).valueOrNull();
}

extern "C" const flat_large::Big* flat_api_borrowed_global(std::uint32_t id,
                                                           const flat_large::Request& request)
{
	return flat_large::borrowedCatalog.callBorrowed<flat_large::Big>(id, request).valueOrNull();
}

// Host assertions use static caller storage. ARM objects retain only the actual
// dispatch paths above, so their individual frames cannot hide this test storage.
#if !defined(__arm__) && !defined(__thumb__)
namespace flat_large {
unsigned checks = 0;
#define CHECK(condition)                                                                           \
	do {                                                                                           \
		++flat_large::checks;                                                                      \
		assert((condition));                                                                       \
	} while (false)

template<class Result, class Invoke, class Access>
void checkOwned(Invoke invoke, Access access)
{
	alignas(Result) static std::byte output[sizeof(Result)];
	const auto before = ownedCalls;
	invoke(output, 0, Request{73});
	auto* result = std::launder(reinterpret_cast<Result*>(output));
	const auto& value = access(*result);
	CHECK(ownedCalls == before + 1);
	CHECK(value.values.front() == 73);
	CHECK(value.values.back() == 0);
	const auto start = reinterpret_cast<std::uintptr_t>(output);
	const auto address = reinterpret_cast<std::uintptr_t>(std::addressof(value));
	CHECK(address >= start);
	CHECK(address + sizeof(Big) <= start + sizeof(output));
	result->~Result();
}

// Result factories permit arbitrary native payloads independently of endpoint
// wire eligibility. Deleted copy/move operations make facade materialization a
// compilation error; self and counters prove exactly one final construction.
struct Immobile {
	const Immobile* self = this;
	std::array<std::uint32_t, (BIG_BYTES - sizeof(void*)) / sizeof(std::uint32_t)> values;
	static inline unsigned constructions = 0, destructions = 0;

	explicit Immobile(std::uint32_t value) noexcept : values{}
	{
		++constructions;
		values.front() = value;
	}

	Immobile(const Immobile&) = delete;
	Immobile(Immobile&&) = delete;

	~Immobile() noexcept
	{
		++destructions;
	}
};

static_assert(sizeof(Immobile) == BIG_BYTES);
static_assert(!std::is_copy_constructible_v<Immobile>);
static_assert(!std::is_move_constructible_v<Immobile>);
} // namespace flat_large

int main()
{
	using namespace flat_large;
	auto manualValue = [](const ts::ServiceResult<Big>& result) -> const Big& {
		CHECK(result.hasValue());
		return result.value();
	};
	auto nativeValue = [](const NativeOwned& result) -> const Big& {
		CHECK(result.hasValue());
		CHECK(result.value().hasValue());
		return result.value().value();
	};
	auto flatValue = [](const FlatOwned& result) -> const Big& {
		CHECK(result.hasValue());
		CHECK(result.status() == ts::ServiceCallStatus::Ok);
		return result.value();
	};
	checkOwned<ts::ServiceResult<Big>>(flat_manual_large_local, manualValue);
	checkOwned<ts::ServiceResult<Big>>(flat_manual_large_global, manualValue);
	checkOwned<NativeOwned>(flat_native_large_local, nativeValue);
	checkOwned<NativeOwned>(flat_native_large_global, nativeValue);
	checkOwned<FlatOwned>(flat_api_large_local, flatValue);
	checkOwned<FlatOwned>(flat_api_large_global, flatValue);
	CHECK(ownedCalls == 6);
	for (auto invoke :
	     {flat_manual_borrowed_local, flat_manual_borrowed_global, flat_native_borrowed_local,
	      flat_native_borrowed_global, flat_api_borrowed_local, flat_api_borrowed_global}) {
		const auto before = borrowedCalls;
		CHECK(invoke(0, Request{}) == std::addressof(backing));
		CHECK(borrowedCalls == before + 1);
		CHECK(invoke(1, Request{}) == nullptr);
		CHECK(borrowedCalls == before + 1);
	}
	CHECK(borrowedCalls == 6);
	CHECK(!ownedTable.callAs<Big>(1, Request{}));
	CHECK(ownedCatalog.callAs<Big>(1, Request{}).status() == ts::ServiceCallStatus::NotFound);
	CHECK(ownedCalls == 6);
	CHECK(ownedTable.callBorrowed<Big>(0, Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(borrowedTable.callAs<Big>(0, Request{}).status() ==
	      ts::ServiceCallStatus::SignatureMismatch);
	CHECK(ownedCalls == 6);
	CHECK(borrowedCalls == 6);
	using ImmobileResult = ts::ServiceCallResult<Immobile>;
	alignas(ImmobileResult) static std::byte finalStorage[sizeof(ImmobileResult)];
	auto* final = ::new (finalStorage) ImmobileResult(ImmobileResult::fromNative([]() {
		return ts::NativeCallResult<ts::ServiceResult<Immobile>>::successFrom([]() {
			return ts::ServiceResult<Immobile>::successFrom([]() {
				return Immobile{91};
			});
		});
	}));
	CHECK(final->hasValue());
	CHECK(final->value().self == std::addressof(final->value()));
	CHECK(final->value().values.front() == 91);
	CHECK(Immobile::constructions == 1);
	CHECK(Immobile::destructions == 0);
	final->~ImmobileResult();
	CHECK(Immobile::destructions == 1);
	std::printf("FlatLargeCodegen: %u checks passed (%u-byte response)\n", checks, BIG_BYTES);
}
#endif

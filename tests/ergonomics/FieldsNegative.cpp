/*
 * @file FieldsNegative.cpp
 * @brief Intended diagnostics for Field read types and borrowed lifetimes.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/field/FieldCatalogs.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>
#include <cstdint>
#include <utility>

namespace ts = telemetry;

struct Big {
	std::array<std::uint32_t, 256> words;
};

struct Other {
	std::array<std::uint32_t, 256> words;
};

struct Owner {
	Big value;
	const Big& get() const noexcept;
	Big own() const noexcept;
};

struct Proxy {
	operator const Big&() const noexcept;
};

enum class LocalPosition {
	First = 0
};

void reject()
{
	Owner owner{};
	volatile Big volatileValue{};
	Proxy proxy;
	(void)volatileValue;
	(void)proxy;
	ts::FieldTable table{ts::field<&Owner::get>("borrowed", owner),
	                     ts::field<&Owner::own>("owning", owner)};
	ts::testing::FieldCatalogTable catalogs{ts::testing::group("group", table)};
	(void)catalogs;
#if CASE == 1
	(void)std::move(table).readBorrowed<Big>(0);
#elif CASE == 2
	(void)std::move(table).readBorrowed<Big, 0>();
#elif CASE == 3
	(void)std::move(catalogs).readBorrowed<Big>(0);
#elif CASE == 4
	(void)std::move(catalogs).readBorrowed<Big, 0>();
#elif CASE == 5
	(void)ts::field<&Owner::get>("temporary owner", Owner{});
#elif CASE == 6
	(void)ts::BorrowedValue<Big>::from(Big{});
#elif CASE == 7
	(void)ts::BorrowedValue<Big>::from(proxy);
#elif CASE == 8
	(void)ts::BorrowedValue<Big>::from(volatileValue);
#elif CASE == 9
	(void)table.readBorrowed<const Big>(0);
#elif CASE == 10
	(void)table.readAsResult<Other, 0>();
#elif CASE == 11
	(void)table.readBorrowed<Big, 1>();
#elif CASE == 12
	(void)table.readBorrowed<Other, 0>();
#elif CASE == 13
	(void)catalogs.readBorrowed<Big>(LocalPosition::First);
#elif CASE == 14
	(void)table.readAsResult<Big>(0.0);
#elif CASE == 15
	(void)table.readAsResult<const Big>(0);
#elif CASE == 16
	(void)sizeof(ts::FieldReadResult<const Big>);
#elif CASE == 17
	(void)ts::FieldReadResult<Big>::successFrom([]() -> Other {
		return {};
	});
#elif CASE == 18
	(void)table.readBorrowed<Big, unsigned>(std::uint64_t{1} << 32);
#elif CASE == 19
	(void)catalogs.readAsResult<Big, unsigned>(std::uint64_t{1} << 32);
#elif CASE == 20
	(void)std::move(table).readAsResult<Big>(0);
#elif CASE == 21
	(void)table.readAsResult<Big, (std::uint64_t{1} << 32)>();
#elif CASE == 22
	(void)ts::testing::CatalogAccess::readAsResult<Big, (std::uint64_t{1} << 32)>(catalogs);
#elif CASE == 23
	(void)std::move(catalogs).readAsResult<Big, 0>();
#elif CASE == 24
	(void)std::move(catalogs).readAsResult<Big>(0);
#else
#error CASE must select one intended rejection
#endif
}

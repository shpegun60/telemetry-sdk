/*
 * @file Catalog.hpp
 * @brief Shared borrowed groups and per-row type references for Model catalogs.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Common borrowed group declarations and per-row structural type references.
 *
 * A group labels a stable local table; declaration order determines its
 * catalog position. TypeId arrays are created later against the complete
 * Model registry, so local tables need no independently assigned global type
 * numbers and carry no semantic parameter metadata.
 */

#ifndef TELEMETRY_MODEL_CATALOG_HPP
#define TELEMETRY_MODEL_CATALOG_HPP
#pragma once

#include "../type/Descriptor.hpp"
#include "../detail/Name.hpp"
#include <concepts>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace telemetry {

namespace detail {

// A declaration-time label and borrowed local-table address: one row of the
// catalog engine under a group tree (and of the testing-only v3.0 producer).
// Construct through tableGroup() so the name is validated and the table
// argument is an actual lvalue. The table and immutable name must outlive all
// catalogs built from this group.
template<class Table>
struct TableGroup {
	using TableType = Table;
	const char* name;
	const Table* table;
};

// The group borrows a stable local table. Its catalog position is its ID.
// A table names its RootTypes; group branches and endpoint definitions do not,
// so the public group(name, items...) in GroupTree.hpp takes those.
template<class... Explicit, class Table>
    requires(sizeof...(Explicit) == 0 && requires { typename Table::RootTypes; })
[[nodiscard]] constexpr auto tableGroup(Name name, Table& table) noexcept
{
	return TableGroup<std::remove_cv_t<Table>>{name, std::addressof(table)};
}

// Compile-time typed access by packed ID into a catalog table: the engine of
// paths and handles (TreePath.hpp) and of the catalog engine's own checks. A
// device names an endpoint by path or handle, so these members are private
// to the catalog tables and reached only through here.
struct CatalogAccess {
	template<auto Id, class Catalogs>
	[[nodiscard]] static constexpr decltype(auto) get(const Catalogs& catalogs) noexcept
	{
		return catalogs.template get<Id>();
	}

	template<auto Id, class Catalogs>
	[[nodiscard]] static auto read(const Catalogs& catalogs) noexcept
	{
		return catalogs.template read<Id>();
	}

	template<auto Id, class Catalogs, class Argument>
	[[nodiscard]] static decltype(auto) write(const Catalogs& catalogs, Argument&& value) noexcept
	{
		return catalogs.template write<Id>(std::forward<Argument>(value));
	}

	template<auto Id, class Catalogs, class... Args>
	[[nodiscard]] static decltype(auto) call(const Catalogs& catalogs, Args&&... args) noexcept
	{
		return catalogs.template call<Id>(std::forward<Args>(args)...);
	}

	template<class To, auto Id, class Catalogs>
	[[nodiscard]] static decltype(auto) readAs(const Catalogs& catalogs) noexcept
	{
		return catalogs.template readAs<To, Id>();
	}

	template<class To, auto Id, class Catalogs>
	[[nodiscard]] static decltype(auto) readBorrowed(const Catalogs& catalogs) noexcept
	{
		return catalogs.template readBorrowed<To, Id>();
	}

	template<class To, auto Id, class Catalogs>
	[[nodiscard]] static decltype(auto) readAsResult(const Catalogs& catalogs) noexcept
	{
		return catalogs.template readAsResult<To, Id>();
	}

	template<auto Id, class Catalogs, class From>
	[[nodiscard]] static decltype(auto) writeAs(const Catalogs& catalogs,
	                                            const From& value) noexcept
	{
		return catalogs.template writeAs<Id>(value);
	}
};

} // namespace detail

// One group of the shared tree (descriptor v3.1): a validated name and its
// items by value. Build it with group(name, items...) from GroupTree.hpp; a
// branch is usually one inline constexpr expression per subsystem, mounted by
// a top-level Model.
template<class... Items>
struct GroupBranch {
	const char* name;
	std::tuple<Items...> items;
};

namespace detail {
template<class T>
inline constexpr bool groupBranch = false;

template<class... Items>
inline constexpr bool groupBranch<GroupBranch<Items...>> = true;
} // namespace detail

// Field and Command each refer to one value/request TypeId per row.
struct ValueTypeCatalog {
	const TypeId* entries;
	std::uint32_t count;
};

} // namespace telemetry

#endif

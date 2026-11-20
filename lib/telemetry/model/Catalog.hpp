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

namespace telemetry {

// A declaration-time label and borrowed local-table address. Construct through
// group() so the name is validated and the table argument is an actual lvalue.
// The table and immutable name must outlive all catalogs built from this group.
template<class Table>
struct TableGroup {
	using TableType = Table;
	const char* name;
	const Table* table;
};

// The group borrows a stable local table. Its catalog position is its ID.
// A table names its RootTypes; group branches and endpoint definitions do not,
// so group(name, items...) in GroupTree.hpp takes those.
template<class... Explicit, class Table>
    requires(sizeof...(Explicit) == 0 && requires { typename Table::RootTypes; })
[[nodiscard]] constexpr auto group(detail::Name name, Table& table) noexcept
{
	return TableGroup<std::remove_cv_t<Table>>{name, std::addressof(table)};
}

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

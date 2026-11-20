/*
 * @file Aggregate.hpp
 * @brief Stable aggregate-reflection facade with exact member access.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Public aggregate-reflection facts isolated from the pinned backend API.
 *
 * Member types, validated names and lvalue access describe a native aggregate
 * without copying it or exposing backend types. Restricting automatic names
 * to ASCII identifiers makes compiler-derived descriptor text predictable;
 * endpoint labels have their separate explicit-name rules.
 */

#ifndef TELEMETRY_AGGREGATE_HPP
#define TELEMETRY_AGGREGATE_HPP
#pragma once

#include "detail/PfrAdapter.hpp"

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

namespace telemetry::reflection {

// Automatic compiler-derived names are deliberately more restricted than
// explicit endpoint/group labels: they must be portable ASCII identifiers.
constexpr bool automaticNameValid(std::string_view name) noexcept
{
	const auto letter = [](unsigned char c) constexpr noexcept {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
	};
	const auto digit = [](unsigned char c) constexpr noexcept {
		return c >= '0' && c <= '9';
	};

	if (name.empty() || !letter(static_cast<unsigned char>(name.front())))
		return false;
	for (char c : name.substr(1)) {
		const auto byte = static_cast<unsigned char>(c);
		if (!letter(byte) && !digit(byte))
			return false;
	}
	return true;
}

// An automatic type name is one ASCII identifier, or two joined by "::"
// (the innermost scope and the type's own name); every part is checked.
constexpr bool automaticScopedNameValid(std::string_view name) noexcept
{
	const auto separator = name.find("::");
	if (separator == std::string_view::npos)
		return automaticNameValid(name);
	return automaticNameValid(name.substr(0, separator)) &&
	       automaticNameValid(name.substr(separator + 2));
}

template<class T>
inline constexpr std::size_t memberCount = detail::pfrMemberCount<T>;

template<std::size_t I, class T>
using MemberType = detail::PfrMemberType<I, T>;

template<std::size_t I, class T>
constexpr std::string_view memberName() noexcept
{
	constexpr auto name = detail::pfrMemberName<I, T>();
	static_assert(automaticNameValid(name),
	              "Automatic reflected member names must be ASCII identifiers");
	return name;
}

template<std::size_t I, class T>
constexpr decltype(auto)
get(T&& object) noexcept(noexcept(detail::pfrGet<I>(std::forward<T>(object))))
{
	// PFR copies members from rvalue aggregates, so the facade accepts only
	// lvalues. Codec access then preserves cv/ref without a hidden copy.
	static_assert(std::is_lvalue_reference_v<T&&>, "reflection::get requires an lvalue aggregate");
	return detail::pfrGet<I>(std::forward<T>(object));
}

} // namespace telemetry::reflection

#endif

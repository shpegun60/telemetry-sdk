/*
 * @file TypeName.hpp
 * @brief The one seam for the wire name of an enum or struct (descriptor v3.1).
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Names a native enum or aggregate for the descriptor's optional type name.
 *
 * typeName<T>() is the only entry point: device code and the type registry
 * never call a backend. The automatic name is the unqualified C++ name, kept
 * only when it is an ASCII identifier, so a template specialization or an
 * unnamed struct is left unnamed, silently and deterministically. An explicit
 * TypeReflection<T> overrides it: typeName("...") replaces the automatic name
 * with any valid UTF-8 label, and noTypeName() removes it. Explicit names win
 * over every automatic backend. Arrays, scalars and Void are never named.
 * Specialize TypeReflection<T> before first use, exactly like EnumReflection.
 */

#ifndef TELEMETRY_TYPE_NAME_HPP
#define TELEMETRY_TYPE_NAME_HPP
#pragma once

#include "Aggregate.hpp"
#include "Enum.hpp"
#include "detail/TypeNameAdapter.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <type_traits>
#include <version>

namespace telemetry::reflection {

// Specialize before the first descriptor that contains T:
//   template<> struct TypeReflection<app::Pair<float>> {
//       static constexpr auto name = typeName("PairF");
//   };
//   template<> struct TypeReflection<app::Internal> {
//       static constexpr auto name = noTypeName();
//   };
template<class T>
struct TypeReflection {};

// The backend dispatch point. Today's backend is magic_enum for enums and the
// compiler's function signature for aggregates. A std::meta backend belongs
// here once a compiler in the SDK's matrix announces both P2996 feature tests;
// it must give the same bytes as today's backend for every type. No std::meta
// code is written before such a compiler can build it.
#if defined(__cpp_impl_reflection) && defined(__cpp_lib_reflection)
inline constexpr bool standardReflectionAvailable = true;
#else
inline constexpr bool standardReflectionAvailable = false;
#endif

namespace detail {

// Explicit type name with owned compile-time bytes.
// Public methods:
// - view(): Borrow the owned name.
template<std::size_t N>
struct ExplicitTypeName {
	std::array<char, N> bytes;

	constexpr std::string_view view() const noexcept
	{
		return {bytes.data(), N - 1};
	}
};

// Removes the automatic name of one type.
struct NoTypeName {};

template<class T>
inline constexpr bool explicitTypeName = false;

template<std::size_t N>
inline constexpr bool explicitTypeName<ExplicitTypeName<N>> = true;

template<class T>
inline constexpr bool stdArray = false;

template<class E, std::size_t N>
inline constexpr bool stdArray<std::array<E, N>> = true;

// Only the two kinds the wire names: enums and aggregates other than std::array.
template<class T>
inline constexpr bool nameableType =
    std::is_enum_v<T> || (std::is_class_v<T> && !stdArray<T> && !std::is_union_v<T>);

template<class T>
consteval std::string_view selectedTypeName() noexcept
{
	if constexpr (requires { TypeReflection<T>::name; }) {
		using Name = std::remove_cvref_t<decltype(TypeReflection<T>::name)>;
		static_assert(explicitTypeName<Name> || std::is_same_v<Name, NoTypeName>,
		              "TypeReflection<T>::name must be typeName(\"...\") or noTypeName()");
		if constexpr (explicitTypeName<Name>)
			return TypeReflection<T>::name.view();
		else
			return {};
	} else {
		// Today's backend; a std::meta backend would be selected here when
		// standardReflectionAvailable, under the same identifier rule.
		std::string_view name{};
		if constexpr (std::is_enum_v<T>)
			name = rawEnumTypeName<T>();
		else
			name = rawStructName<T>();
		return automaticNameValid(name) ? name : std::string_view{};
	}
}

// Owns exactly the selected bytes, so a view borrows this short array and not
// the compiler's long function signature or a backend's storage.
template<class T>
struct TypeNameStorage {
	static constexpr std::size_t size = selectedTypeName<T>().size();
	static constexpr std::array<char, size + 1> bytes = [] {
		std::array<char, size + 1> result{};
		constexpr auto source = selectedTypeName<T>();
		for (std::size_t i = 0; i < size; ++i)
			result[i] = source[i];
		return result;
	}();
};

} // namespace detail

// Own the name bytes, including when the caller used a temporary/local array,
// and validate them like an explicit enum entry: 1..4096 bytes of UTF-8, no NUL.
template<class Char, std::size_t N>
    requires(std::is_same_v<Char, char> || std::is_same_v<Char, char8_t>)
consteval auto typeName(const Char (&name)[N])
{
	static_assert(N > 1 && N <= 4097, "Type name length is outside 1..4096 bytes");
	detail::ExplicitTypeName<N> result{};
	for (std::size_t i = 0; i < N; ++i) {
		if constexpr (std::is_same_v<Char, char>)
			result.bytes[i] = name[i];
		else
			result.bytes[i] = std::bit_cast<char>(static_cast<std::uint8_t>(name[i]));
	}
	if (result.bytes[N - 1] != 0 || !detail::validUtf8(result.view()))
		std::abort(); // Invalid UTF-8 or embedded NUL fails constant evaluation.
	return result;
}

// Leave one enum or struct unnamed, for example to settle a short-name clash
// under the descriptor's uniqueness rule without inventing a label.
consteval detail::NoTypeName noTypeName() noexcept
{
	return {};
}

// The wire name of T, or an empty view when T is unnamed, an array, a scalar
// or Void. Callable at run time; the view borrows static storage of exactly
// the name's length. It costs nothing in an image unless it is used.
template<class T>
constexpr std::string_view typeName() noexcept
{
	using Normal = std::remove_cvref_t<T>;
	if constexpr (detail::nameableType<Normal>) {
		using Storage = detail::TypeNameStorage<Normal>;
		return {Storage::bytes.data(), Storage::size};
	} else {
		static_assert(
		    !requires { TypeReflection<Normal>::name; },
		    "TypeReflection names apply to enums and structs only");
		return {};
	}
}

} // namespace telemetry::reflection

#endif

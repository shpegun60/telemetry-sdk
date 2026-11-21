/*
 * @file Traits.hpp
 * @brief Supported fixed wire shapes, sizes and compile-time resource limits.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Validates the supported native type graph and computes its fixed wire shape.
 *
 * Wire size sums semantic members rather than native padding. Recursive byte,
 * depth and expanded-node limits bound generated metadata and value traversal.
 * Unsupported representations fail during template instantiation, before the
 * codec or a registry can depend on an unsafe native layout.
 */

#ifndef TELEMETRY_TYPE_TRAITS_HPP
#define TELEMETRY_TYPE_TRAITS_HPP
#pragma once

#include "WireTypes.hpp"
#include "../reflection/Reflection.hpp"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace telemetry {

namespace detail {

// Unsupported is a compile-time classification only. It is never a wire kind.
enum class TypeClass : std::uint8_t {
	Void,
	Scalar,
	Enum,
	Struct,
	Array,
	Unsupported
};

// Recognizes std::array and exposes its element type and extent.
template<class T>
struct ArrayInfo {
	static constexpr bool isArray = false;
};

// Recognizes std::array and exposes its element type and extent.
template<class E, std::size_t N>
struct ArrayInfo<std::array<E, N>> {
	static constexpr bool isArray = true;
	using Element = E;
	static constexpr std::size_t count = N;
};

template<class T>
inline constexpr bool scalar = std::is_same_v<T, bool> || std::is_same_v<T, float> ||
                               std::is_same_v<T, double> || reflection::detail::integerWire<T>;

template<class T>
consteval TypeClass classify()
{
	if constexpr (std::is_void_v<T>)
		return TypeClass::Void;
	else if constexpr (scalar<T>)
		return TypeClass::Scalar;
	else if constexpr (std::is_enum_v<T>)
		return TypeClass::Enum;
	else if constexpr (ArrayInfo<T>::isArray)
		return TypeClass::Array;
	else if constexpr (std::is_class_v<T>)
		return TypeClass::Struct;
	else
		return TypeClass::Unsupported;
}

template<class T>
consteval ScalarCode scalarCode()
{
	if constexpr (std::is_same_v<T, bool>)
		return ScalarCode::Bool;
	else if constexpr (std::is_same_v<T, float>)
		return ScalarCode::F32;
	else if constexpr (std::is_same_v<T, double>)
		return ScalarCode::F64;
	else if constexpr (sizeof(T) == 1)
		return std::is_signed_v<T> ? ScalarCode::S8 : ScalarCode::U8;
	else if constexpr (sizeof(T) == 2)
		return std::is_signed_v<T> ? ScalarCode::S16 : ScalarCode::U16;
	else if constexpr (sizeof(T) == 4)
		return std::is_signed_v<T> ? ScalarCode::S32 : ScalarCode::U32;
	else
		return std::is_signed_v<T> ? ScalarCode::S64 : ScalarCode::U64;
}

template<class T, TypeClass K = classify<T>()>
struct TypeInfo;

// Checked fixed wire facts for this native type.
template<class T>
struct TypeInfo<T, TypeClass::Unsupported> {
	static_assert(
	    !std::is_same_v<T, T>,
	    "Unsupported structured wire type: use a fixed scalar, scoped enum, std::array or aggregate");
	// These values are unreachable: the assertion above always rejects T.
	// Supplying them keeps dependent diagnostics focused on that assertion.
	static constexpr TypeKind kind = TypeKind::Scalar;
	static constexpr std::uint32_t wireSize = 0;
	static constexpr std::uint32_t depth = 0;
	static constexpr std::uint32_t expandedNodes = 0;
	static constexpr std::string_view name{};
};

// Checked fixed wire facts for this native type.
template<class T>
struct TypeInfo<T, TypeClass::Void> {
	static constexpr TypeKind kind = TypeKind::Void;
	static constexpr std::uint32_t wireSize = 0;
	static constexpr std::uint32_t depth = 0;
	static constexpr std::uint32_t expandedNodes = 0;
	static constexpr std::string_view name{};
};

// Checked fixed wire facts for this native type.
template<class T>
struct TypeInfo<T, TypeClass::Scalar> {
	static_assert(CHAR_BIT == 8, "Structured wire bytes require 8-bit char");
	static constexpr TypeKind kind = TypeKind::Scalar;
	static constexpr ScalarCode code = scalarCode<T>();
	static constexpr std::uint32_t wireSize = std::is_same_v<T, bool> ? 1 : sizeof(T);
	static constexpr std::uint32_t depth = 0;
	static constexpr std::uint32_t expandedNodes = 1;
	static constexpr std::string_view name{}; // Builtin labels stay host words.
	static_assert(!std::is_integral_v<T> || std::is_same_v<T, bool> ||
	                  (std::numeric_limits<T>::radix == 2 &&
	                   std::numeric_limits<T>::digits ==
	                       static_cast<int>(sizeof(T) * 8 - (std::is_signed_v<T> ? 1 : 0))),
	              "Structured integers require full-width binary representation");
	static_assert(!std::is_integral_v<T> || !std::is_signed_v<T> ||
	                  std::numeric_limits<T>::lowest() == -std::numeric_limits<T>::max() - 1,
	              "Structured signed integers require two's-complement range");
	static_assert(!std::is_same_v<T, float> ||
	                  (sizeof(float) == 4 && std::numeric_limits<float>::is_iec559 &&
	                   std::numeric_limits<float>::digits == 24),
	              "F32 requires IEEE binary32");
	static_assert(!std::is_same_v<T, double> ||
	                  (sizeof(double) == 8 && std::numeric_limits<double>::is_iec559 &&
	                   std::numeric_limits<double>::digits == 53),
	              "F64 requires IEEE binary64");
};

// Checked fixed wire facts for this native type.
template<class T>
struct TypeInfo<T, TypeClass::Enum> {
	static constexpr TypeKind kind = TypeKind::Enum;
	using EnumType = reflection::Enum<T>;
	using Underlying = typename EnumType::Underlying;
	static constexpr std::uint32_t wireSize = TypeInfo<Underlying>::wireSize;
	static constexpr std::uint32_t depth = 0;
	static constexpr std::uint32_t expandedNodes = 1;
	static_assert(EnumType::entryCount <= 65536, "Enum dictionary exceeds model ceiling");
	// Wire type name (descriptor v3.1), empty when unnamed. Evaluated on use.
	static constexpr std::string_view name = reflection::typeName<T>();
};

// Checked fixed wire facts for this native type.
template<class T>
struct TypeInfo<T, TypeClass::Array> {
	using Element = typename ArrayInfo<T>::Element;
	static constexpr std::size_t count = ArrayInfo<T>::count;
	static_assert(count <= Limits::maxArrayElements, "Array has too many elements");
	static_assert(!std::is_const_v<Element> && !std::is_volatile_v<Element> &&
	                  !std::is_reference_v<Element>,
	              "Array element must be mutable value type");
	static_assert(TypeInfo<Element>::kind != TypeKind::Void, "Array cannot contain Void");
	static constexpr TypeKind kind = TypeKind::Array;
	static constexpr std::uint64_t wideBytes = std::uint64_t{count} * TypeInfo<Element>::wireSize;
	static_assert(wideBytes <= std::numeric_limits<std::uint32_t>::max() &&
	                  wideBytes <= Limits::maxValueWireBytes,
	              "Array wire size exceeds supported limit");
	static constexpr std::uint32_t wireSize = static_cast<std::uint32_t>(wideBytes);
	static constexpr std::uint32_t depth = TypeInfo<Element>::depth + 1;
	static_assert(depth <= Limits::maxTypeDepth, "Type nesting depth exceeds supported limit");
	static constexpr std::uint64_t wideNodes =
	    1 + std::uint64_t{count} * TypeInfo<Element>::expandedNodes;
	static_assert(wideNodes <= Limits::maxExpandedValueNodes,
	              "Array expanded nodes exceed supported limit");
	static constexpr std::uint32_t expandedNodes = static_cast<std::uint32_t>(wideNodes);
	static constexpr std::string_view name{}; // The host composes Mode[4], Config[2].
};

// Validate every semantic member and sum its wire shape. Native padding is
// never serialized, packed members are rejected, and std::array is the explicit
// repeated-value form. Reflection inspects type/layout facts without constructing
// a runtime application object or evaluating default member initializers.
// Public methods:
// - byteSum(): Sum member extents.
// - nodeSum(): Sum expanded nodes.
// - maxDepth(): Bound member nesting.
template<class T>
struct TypeInfo<T, TypeClass::Struct> {
	static constexpr bool shapeValid = std::is_aggregate_v<T> && std::is_standard_layout_v<T> &&
	                                   std::is_trivially_copyable_v<T> &&
	                                   std::is_trivially_destructible_v<T>;
	static_assert(shapeValid, "Structured wire structs must be standard-layout trivial aggregates");
	static constexpr TypeKind kind = TypeKind::Struct;
	static constexpr std::size_t memberCount = [] {
		if constexpr (shapeValid)
			return telemetry::reflection::memberCount<T>;
		else
			return std::size_t{0};
	}();
	static_assert(memberCount <= Limits::maxStructMembers, "Struct has too many members");

	// Checked aggregate-member shape used by recursive wire facts.
	// Public methods (Clang alignment check):
	// - addressAligned(): Check member alignment.
	template<std::size_t I>
	struct Member {
		using Reflected = telemetry::reflection::MemberType<I, T>;
		static_assert(!std::is_const_v<Reflected> && !std::is_volatile_v<Reflected> &&
		                  !std::is_reference_v<Reflected> && !std::is_array_v<Reflected>,
		              "Structured members must be mutable value types; use std::array");
		using Native = std::remove_cv_t<Reflected>;
		static_assert(alignof(T) >= alignof(Native),
		              "Packed aggregate member alignment is unsupported");
#if defined(__clang__)
		// Clang may form a reference to a packed member even when PFR's GCC
		// backend rejects it. Inspect the member address without constructing
		// an application object, including aggregates with non-constexpr DMI.
		static consteval bool addressAligned()
		{
			return telemetry::reflection::detail::pfrMemberAligned<I, T>();
		}

		static_assert(addressAligned(), "Packed aggregate member alignment is unsupported");
#endif
		static_assert(TypeInfo<Native>::kind != TypeKind::Void, "Struct member cannot be Void");
		static constexpr auto name = telemetry::reflection::memberName<I, T>();
		static_assert(name.size() <= 4096, "Struct member name exceeds model ceiling");
		static constexpr std::uint32_t bytes = TypeInfo<Native>::wireSize;
		static constexpr std::uint32_t nodes = TypeInfo<Native>::expandedNodes;
		static constexpr std::uint32_t depth = TypeInfo<Native>::depth;
	};

	template<std::size_t... I>
	static consteval std::uint64_t byteSum(std::index_sequence<I...>)
	{
		return (std::uint64_t{0} + ... + Member<I>::bytes);
	}

	template<std::size_t... I>
	static consteval std::uint64_t nodeSum(std::index_sequence<I...>)
	{
		return (std::uint64_t{1} + ... + Member<I>::nodes);
	}

	template<std::size_t... I>
	static consteval std::uint32_t maxDepth(std::index_sequence<I...>)
	{
		std::uint32_t result = 0;
		((result = result < Member<I>::depth ? Member<I>::depth : result), ...);
		return result;
	}

	static constexpr std::uint64_t wideBytes = byteSum(std::make_index_sequence<memberCount>{});
	static_assert(wideBytes <= std::numeric_limits<std::uint32_t>::max() &&
	                  wideBytes <= Limits::maxValueWireBytes,
	              "Struct wire size exceeds supported limit");
	static constexpr std::uint32_t wireSize = static_cast<std::uint32_t>(wideBytes);
	static constexpr std::uint32_t depth = 1 + maxDepth(std::make_index_sequence<memberCount>{});
	static_assert(depth <= Limits::maxTypeDepth, "Type nesting depth exceeds supported limit");
	static constexpr std::uint64_t wideNodes = nodeSum(std::make_index_sequence<memberCount>{});
	static_assert(wideNodes <= Limits::maxExpandedValueNodes,
	              "Struct expanded nodes exceed supported limit");
	static constexpr std::uint32_t expandedNodes = static_cast<std::uint32_t>(wideNodes);
	// Wire type name (descriptor v3.1), empty when unnamed. Evaluated on use.
	static constexpr std::string_view name = reflection::typeName<T>();
};

} // namespace detail

// Public normalized facts for one supported native type. Instantiating Type<T>
// triggers representation and recursive-limit checks; wireSize<T> is a fixed
// canonical extent independent of native aggregate padding.
template<class T>
using Type = detail::TypeInfo<std::remove_cvref_t<T>>;

template<class T>
inline constexpr TypeKind typeKind = Type<T>::kind;

template<class T>
inline constexpr std::uint32_t wireSize = Type<T>::wireSize;

template<class T>
inline constexpr std::uint32_t expandedNodes = Type<T>::expandedNodes;

} // namespace telemetry

#endif

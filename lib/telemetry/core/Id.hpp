/**
 * @file Id.hpp
 * @brief Shared packed, constant-time field and command identifiers.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see ../LICENSE.
 */

/*
 * Positional endpoint identifiers with validation before narrowing conversions.
 *
 * A packed identifier contains a 16-bit group and a 16-bit local position.
 * The helpers preserve the original integer width until it has been checked,
 * so wide transport input cannot wrap to a different valid endpoint. Fallible
 * runtime helpers and compile-time diagnostics serve different call sites.
 */

#ifndef TELEMETRY_ID_H
#define TELEMETRY_ID_H
#pragma once

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <type_traits>

namespace telemetry {

// Packed identity: high 16 bits are the zero-based group position; low
// 16 bits are the zero-based entry position within that group. Fields and
// commands use separate logical spaces with the same representation.
using PackedId = std::uint32_t;
using FieldId = PackedId;
using CommandId = PackedId;
using GroupId = std::uint16_t;
using EntryOffset = std::uint16_t;
using FieldOffset = EntryOffset;
using CommandOffset = EntryOffset;
inline constexpr std::uint32_t idComponentCapacity = 65536u;

namespace detail {
template<class T>
inline constexpr bool isIdInput = std::is_integral_v<T> || std::is_enum_v<T>;
// Local positions may use scoped enums. Packed IDs have no enum provenance:
// accepting a local position enum here would silently select group zero.
template<class T>
inline constexpr bool isPackedIdInput = std::is_integral_v<T>;

// Check the original value before converting to a packed ID or a native
// position. On ARM a uint64_t transport value must not wrap through size_t.
template<class To, class From>
constexpr bool indexFits(From value) noexcept
{
	static_assert(std::is_integral_v<To> && std::is_unsigned_v<To>);
	static_assert(isIdInput<From>, "Telemetry identifiers require integers or enums");
	if constexpr (std::is_enum_v<From>) {
		return indexFits<To>(static_cast<std::underlying_type_t<From>>(value));
	} else {
		if constexpr (std::is_signed_v<From>) {
			if (value < 0)
				return false;
		}
		if constexpr (std::numeric_limits<From>::digits > std::numeric_limits<To>::digits)
			return value <= static_cast<From>(std::numeric_limits<To>::max());
		else
			return true;
	}
}

// No packed ID is spare: every 32-bit value names a possible position. Packing
// and decomposition treat invalid input as a contract violation instead of
// manufacturing an ID or component that could name a different row.
[[noreturn]] inline void invalidIdComponent() noexcept
{
	std::abort();
}

template<auto Id>
constexpr PackedId packedIdValue() noexcept
{
	using T = std::remove_cv_t<decltype(Id)>;
	static_assert(isPackedIdInput<T>,
	              "Packed ID must be an integer; pack local enum positions with makeId");
	if constexpr (isPackedIdInput<T>) {
		static_assert(indexFits<PackedId>(Id), "Packed ID is outside the 32-bit range");
		return static_cast<PackedId>(Id);
	} else
		return 0;
}

// Preserve a template position at full width until the owning table checks
// its actual size. Casting an enum straight to size_t could wrap a U64 code
// to a valid row on ARM32. This helper is evaluated only at compile time.
template<auto Position>
constexpr std::uintmax_t positionValue() noexcept
{
	using T = std::remove_cv_t<decltype(Position)>;
	static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
	              "Typed table position must be an integer or enum");
	if constexpr (std::is_enum_v<T>) {
		return positionValue<static_cast<std::underlying_type_t<T>>(Position)>();
	} else if constexpr (std::is_integral_v<T>) {
		static_assert(sizeof(T) <= sizeof(std::uintmax_t),
		              "Typed table position is wider than uintmax_t");
		if constexpr (std::is_signed_v<T>)
			static_assert(Position >= 0, "Typed table position must be non-negative");
		return static_cast<std::uintmax_t>(Position);
	} else
		return 0; // Keep invalid types out of subsequent casts/instantiations.
}
} // namespace detail

// The packing helpers are internal. A device names an endpoint by its path
// (idOf<model, "adc/Gain">()) or a handle (.id); the PackedId stays on the
// wire. The engine, the generated code's checks and the checks of the ID
// packing itself call detail::makeId and detail::tryMakeId.
namespace detail {

// The narrow overload remains just a shift and OR. Lookup separately checks
// the actual catalog bounds.
constexpr PackedId makeId(GroupId group, EntryOffset index) noexcept
{
	return (PackedId{group} << 16) | PackedId{index};
}

// Use this form at transport boundaries. An invalid component cannot alias a
// different entry; no conversion takes place until both bounds have passed.
template<class... Explicit, class Group, class Position,
         std::enable_if_t<sizeof...(Explicit) == 0 && detail::isIdInput<Group> &&
                              detail::isIdInput<Position>,
                          int> = 0>
constexpr std::optional<PackedId> tryMakeId(Group group, Position index) noexcept
{
	if (!detail::indexFits<GroupId>(group) || !detail::indexFits<EntryOffset>(index))
		return std::nullopt;
	return makeId(static_cast<GroupId>(group), static_cast<EntryOffset>(index));
}

// Guaranteed compile-time diagnostics for statically chosen components, even
// when the surrounding expression is evaluated at runtime.
template<auto Group, auto Position>
constexpr PackedId makeId() noexcept
{
	static_assert(detail::isIdInput<decltype(Group)> && detail::isIdInput<decltype(Position)>,
	              "Static identifier components require integers or enums");
	if constexpr (detail::isIdInput<decltype(Group)> && detail::isIdInput<decltype(Position)>) {
		static_assert(detail::indexFits<GroupId>(Group),
		              "Static group is outside the 16-bit ID range");
		static_assert(detail::indexFits<EntryOffset>(Position),
		              "Static position is outside the 16-bit ID range");
		return makeId(static_cast<GroupId>(Group), static_cast<EntryOffset>(Position));
	} else
		return 0;
}

// Preserve full-width integer literals and variables until validation. Invalid
// constant expressions fail to compile; invalid runtime components terminate.
// A literal in an ordinary call is not necessarily a constant expression; use
// makeId<Group, Position>() for guaranteed diagnostics, or tryMakeId() for
// fallible runtime input.
template<class... Explicit, class Group, class Position,
         std::enable_if_t<sizeof...(Explicit) == 0 && detail::isIdInput<Group> &&
                              detail::isIdInput<Position>,
                          int> = 0>
constexpr PackedId makeId(Group group, Position index) noexcept
{
	const auto id = tryMakeId(group, index);
	return id ? *id : (detail::invalidIdComponent(), PackedId{});
}

// Prevent floating or implicitly-convertible wrapper values from bypassing the
// full-width check by selecting the uint16_t overload.
template<class... Explicit, class Group, class Position,
         std::enable_if_t<sizeof...(Explicit) == 0 &&
                              (!detail::isIdInput<Group> || !detail::isIdInput<Position>),
                          int> = 0>
PackedId makeId(Group, Position) = delete;

} // namespace detail

template<class... Explicit, class Id,
         std::enable_if_t<sizeof...(Explicit) == 0 && detail::isPackedIdInput<Id>, int> = 0>
constexpr std::optional<GroupId> tryGroupOf(Id id) noexcept
{
	return detail::indexFits<PackedId>(id)
	           ? std::optional<GroupId>{static_cast<GroupId>(static_cast<PackedId>(id) >> 16)}
	           : std::nullopt;
}

template<class... Explicit, class Id,
         std::enable_if_t<sizeof...(Explicit) == 0 && detail::isPackedIdInput<Id>, int> = 0>
constexpr std::optional<EntryOffset> tryIndexOf(Id id) noexcept
{
	return detail::indexFits<PackedId>(id) ? std::optional<EntryOffset>{static_cast<EntryOffset>(
	                                             static_cast<PackedId>(id) & 0xffffu)}
	                                       : std::nullopt;
}

template<class... Explicit, class Id,
         std::enable_if_t<sizeof...(Explicit) == 0 && detail::isPackedIdInput<Id>, int> = 0>
constexpr GroupId groupOf(Id id) noexcept
{
	return detail::indexFits<PackedId>(id) ? static_cast<GroupId>(static_cast<PackedId>(id) >> 16)
	                                       : (detail::invalidIdComponent(), GroupId{});
}

template<class... Explicit, class Id,
         std::enable_if_t<sizeof...(Explicit) == 0 && detail::isPackedIdInput<Id>, int> = 0>
constexpr EntryOffset indexOf(Id id) noexcept
{
	return detail::indexFits<PackedId>(id)
	           ? static_cast<EntryOffset>(static_cast<PackedId>(id) & 0xffffu)
	           : (detail::invalidIdComponent(), EntryOffset{});
}

} // namespace telemetry

#endif

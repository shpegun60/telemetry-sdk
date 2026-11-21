/*
 * @file Registry.hpp
 * @brief Compile-time, dependency-first registry of exact C++ wire types.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Assigns Model-wide identifiers to exact C++ types and their dependencies.
 *
 * Built-in positions are fixed; native scalar spellings share their canonical
 * wire ID. User types follow deterministic first use with dependencies recorded
 * first. Exact type identity deduplicates user roots even when different types
 * have the same wire shape. Static descriptor storage
 * contains structural facts only and never references application owners.
 */

#ifndef TELEMETRY_TYPE_REGISTRY_HPP
#define TELEMETRY_TYPE_REGISTRY_HPP
#pragma once

#include "Descriptor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace telemetry {
namespace detail {

// Ordered compile-time type sequence with its element count.
template<class... T>
struct TypeList {
	static constexpr std::size_t size = sizeof...(T);
};

template<class... Lists>
struct ConcatLists;

// Concatenates compile-time type sequences without reordering them.
template<>
struct ConcatLists<> {
	using type = TypeList<>;
};

// Concatenates compile-time type sequences without reordering them.
template<class... First, class... Rest, class... Tail>
struct ConcatLists<TypeList<First...>, TypeList<Rest...>, Tail...>
    : ConcatLists<TypeList<First..., Rest...>, Tail...> {};

// Concatenates compile-time type sequences without reordering them.
template<class... T>
struct ConcatLists<TypeList<T...>> {
	using type = TypeList<T...>;
};

template<class List>
struct RegistryFromList;

template<class List, class T>
struct Contains;

// Tests exact type membership in an ordered type sequence.
template<class... Present, class T>
struct Contains<TypeList<Present...>, T> : std::bool_constant<(std::is_same_v<Present, T> || ...)> {
};

template<class List, class T>
struct Append;

// Adds one type at the end of an ordered type sequence.
template<class... Present, class T>
struct Append<TypeList<Present...>, T> {
	using type = TypeList<Present..., T>;
};

// Membership markers avoid a fold over all previously discovered roots.
// The set contains normalized, unique types, so no base can occur twice.
template<class T>
struct RegistryRootMarker {};

template<class List>
struct RegistryRootSet;

template<class... T>
struct RegistryRootSet<TypeList<T...>> : RegistryRootMarker<T>... {};

template<class List, class T,
         bool Present = std::is_base_of_v<RegistryRootMarker<T>, RegistryRootSet<List>>>
struct UniqueRootAppend {
	using type = List;
};

template<class List, class T>
struct UniqueRootAppend<List, T, false> : Append<List, T> {};

// This recursion is confined to one block of at most 32 input roots.
template<class List, class... Roots>
struct UniqueRootBlock;

template<class List>
struct UniqueRootBlock<List> {
	using type = List;
};

template<class List, class First, class... Rest>
struct UniqueRootBlock<List, First, Rest...> {
	using WithFirst = typename UniqueRootAppend<List, std::remove_cvref_t<First>>::type;
	using type = typename UniqueRootBlock<WithFirst, Rest...>::type;
};

// Long sequences advance by a fixed-size block rather than by each endpoint.
// The block size is private compile-time work granularity, not an API limit
// or a runtime storage budget. No giant tuple is instantiated to split it.
template<class List, class... Roots>
struct UniqueRootChunks : UniqueRootBlock<List, Roots...> {};

template<class List, class T0, class T1, class T2, class T3, class T4, class T5, class T6, class T7,
         class T8, class T9, class T10, class T11, class T12, class T13, class T14, class T15,
         class T16, class T17, class T18, class T19, class T20, class T21, class T22, class T23,
         class T24, class T25, class T26, class T27, class T28, class T29, class T30, class T31,
         class... Rest>
struct UniqueRootChunks<List, T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15,
                        T16, T17, T18, T19, T20, T21, T22, T23, T24, T25, T26, T27, T28, T29, T30,
                        T31, Rest...> {
	using WithBlock =
	    typename UniqueRootBlock<List, T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13,
	                             T14, T15, T16, T17, T18, T19, T20, T21, T22, T23, T24, T25, T26,
	                             T27, T28, T29, T30, T31>::type;
	using type = typename UniqueRootChunks<WithBlock, Rest...>::type;
};

template<class List>
struct UniqueFirst;

// Registry inputs preserve first occurrence and exact normalized C++ identity.
// Positional RootTypes must remain separate: their duplicates describe rows.
template<class... Roots>
struct UniqueFirst<TypeList<Roots...>> : UniqueRootChunks<TypeList<>, Roots...> {};

// Model also accepts catalog-like adapters written before RegistryRootTypes
// existed. Preserve their source contract while deduplicating their old roots.
template<class Source, class = void>
struct RegistryRootsOf : UniqueFirst<typename Source::RootTypes> {};

template<class Source>
struct RegistryRootsOf<Source, std::void_t<typename Source::RegistryRootTypes>> {
	using type = typename Source::RegistryRootTypes;
};

template<class T, class... Present>
struct Position;

// Finds an exact type position or diagnoses an absent type.
template<class T, class First, class... Rest>
struct Position<T, First, Rest...> {
	static constexpr TypeId value = [] {
		if constexpr (std::is_same_v<T, First>)
			return TypeId{0};
		else
			return TypeId{1} + Position<T, Rest...>::value;
	}();
};

// Finds an exact type position or diagnoses an absent type.
template<class T>
struct Position<T> {
	static_assert(!std::is_same_v<T, T>, "Type is absent from this TypeRegistry");
};

template<class List, class T>
struct Collect;

// Collects aggregate member types in declaration order.
template<class List, class T, std::size_t Index, std::size_t Count>
struct CollectMembers {
	using Member = typename Type<T>::template Member<Index>;
	using WithMember = typename Collect<List, typename Member::Native>::type;
	using type = typename CollectMembers<WithMember, T, Index + 1, Count>::type;
};

// Collects aggregate member types in declaration order.
template<class List, class T, std::size_t Count>
struct CollectMembers<List, T, Count, Count> {
	using type = List;
};

// Collects the dependencies required by one structural kind.
template<class List, class T, TypeKind Kind = Type<T>::kind>
struct CollectDependencies {
	using type = List;
};

// Collects the dependencies required by one structural kind.
template<class List, class T>
struct CollectDependencies<List, T, TypeKind::Enum> {
	using type = typename Collect<List, typename Type<T>::Underlying>::type;
};

// Collects the dependencies required by one structural kind.
template<class List, class T>
struct CollectDependencies<List, T, TypeKind::Array> {
	using type = typename Collect<List, typename Type<T>::Element>::type;
};

// Collects the dependencies required by one structural kind.
template<class List, class T>
struct CollectDependencies<List, T, TypeKind::Struct> {
	using type = typename CollectMembers<List, T, 0, Type<T>::memberCount>::type;
};

// Dependencies are collected before the containing type. Once present, the
// exact normalized C++ type is not traversed again; equal wire shape alone
// does not merge two different C++ types.
template<class List, class T,
         bool Present = Contains<List, T>::value || Type<T>::kind == TypeKind::Scalar>
struct CollectOne {
	using type = List;
};

// Keeps an existing type or collects and appends a new type.
template<class List, class T>
struct CollectOne<List, T, false> {
	// Force all of Type<T>'s representation, depth and byte-limit checks even
	// when a caller only asks for the registry's type order.
	static_assert(Type<T>::wireSize <= Limits::maxValueWireBytes);
	using WithDependencies = typename CollectDependencies<List, T>::type;
	using type = typename Append<WithDependencies, T>::type;
};

// Collects a normalized type once after its dependencies.
template<class List, class T>
struct Collect : CollectOne<List, std::remove_cvref_t<T>> {};

template<class List, class... Roots>
struct CollectRoots;

// Collects root types in deterministic first-use order.
template<class List>
struct CollectRoots<List> {
	using type = List;
};

// Collects root types in deterministic first-use order.
template<class List, class First, class... Rest>
struct CollectRoots<List, First, Rest...> {
	using WithFirst = typename Collect<List, First>::type;
	using type = typename CollectRoots<WithFirst, Rest...>::type;
};

// These exact positions are part of the descriptor v3.0 contract.
using Builtins = TypeList<Void, bool, std::uint8_t, std::int8_t, std::uint16_t, std::int16_t,
                          std::uint32_t, std::int32_t, std::uint64_t, std::int64_t, float, double>;

} // namespace detail

// Roots are supplied in the same first-use order as the eventual Model:
// Fields, Commands, then Services (request before response). A local table
// cannot assign global TypeIds before all roots are known.
// Public methods:
// - typeId(): Resolve exact type.
// - view(): Borrow type metadata.
// - descriptor(): Borrow typed descriptor.
template<class... Roots>
class TypeRegistry {
	using Ordered = typename detail::CollectRoots<detail::Builtins, Roots...>::type;

public:
	static_assert(Ordered::size <= Limits::maxTypeCount, "Type registry exceeds model ceiling");
	static constexpr std::uint32_t typeCount = static_cast<std::uint32_t>(Ordered::size);

	template<class T>
	[[nodiscard]] static consteval TypeId typeId() noexcept
	{
		using Normal = std::remove_cvref_t<T>;
		if constexpr (Type<Normal>::kind == TypeKind::Scalar) {
			// Scalar IDs are reserved by wire representation, independently of
			// whether int64_t names long or long long on the native ABI.
			return static_cast<TypeId>(Type<Normal>::code);
		} else if constexpr (detail::Contains<Ordered, Normal>::value) {
			return []<class... Present>(detail::TypeList<Present...>) {
				return detail::Position<Normal, Present...>::value;
			}(Ordered{});
		} else {
			static_assert(detail::Contains<Ordered, Normal>::value,
			              "Type is absent from this TypeRegistry");
			return TypeId{0};
		}
	}

private:
	// Static auxiliary arrays belong to this registry specialization, so view
	// pointers never depend on a runtime Registry/Model object's address.
	// Public methods:
	// - recordSize(): Calculate metadata extent.
	// - make(): Build type descriptor.
	template<class T>
	struct Stored {
		inline static constexpr auto members = [] {
			if constexpr (Type<T>::kind == TypeKind::Struct) {
				return []<std::size_t... I>(std::index_sequence<I...>) {
					return std::array<MemberDescriptor, sizeof...(I)>{
					    MemberDescriptor{typeId<typename Type<T>::template Member<I>::Native>(),
					                     Type<T>::template Member<I>::name}...};
				}(std::make_index_sequence<Type<T>::memberCount>{});
			} else {
				return std::array<MemberDescriptor, 0>{};
			}
		}();

		inline static constexpr auto enumEntries = [] {
			if constexpr (Type<T>::kind == TypeKind::Enum) {
				using Raw = typename Type<T>::Underlying;
				using Unsigned = std::make_unsigned_t<Raw>;
				return []<std::size_t... I>(std::index_sequence<I...>) {
					return std::array<EnumEntryDescriptor, sizeof...(I)>{EnumEntryDescriptor{
					    static_cast<std::uint64_t>(static_cast<Unsigned>(
					        static_cast<Raw>(reflection::Enum<T>::template entryValue<I>()))),
					    reflection::Enum<T>::template entryName<I>()}...};
				}(std::make_index_sequence<reflection::Enum<T>::entryCount>{});
			} else {
				return std::array<EnumEntryDescriptor, 0>{};
			}
		}();

		[[nodiscard]] static consteval std::uint64_t recordSize() noexcept
		{
			// Eight bytes of record header and twelve bytes of type prefix.
			std::uint64_t bytes = 20;
			if constexpr (Type<T>::kind == TypeKind::Scalar) {
				bytes += 4;
			} else if constexpr (Type<T>::kind == TypeKind::Enum) {
				bytes += 8;
				for (const auto& entry : enumEntries)
					bytes += Type<T>::wireSize + 4 + entry.name.size();
			} else if constexpr (Type<T>::kind == TypeKind::Struct) {
				bytes += 4;
				for (const auto& member : members)
					bytes += 8 + member.name.size();
			} else if constexpr (Type<T>::kind == TypeKind::Array) {
				bytes += 8;
			}
			return bytes;
		}

		static constexpr std::uint64_t wideRecordBytes = recordSize();
		static_assert(wideRecordBytes <= std::numeric_limits<std::uint32_t>::max(),
		              "Type record byte count overflows u32");

		[[nodiscard]] static consteval TypeDescriptor make() noexcept
		{
			TypeDescriptor result{};
			result.id = typeId<T>();
			result.kind = Type<T>::kind;
			result.wireBytes = Type<T>::wireSize;
			result.recordBytes = static_cast<std::uint32_t>(wideRecordBytes);
			if constexpr (Type<T>::kind == TypeKind::Scalar) {
				result.scalarCode = Type<T>::code;
			} else if constexpr (Type<T>::kind == TypeKind::Enum) {
				result.relatedTypeId = typeId<typename Type<T>::Underlying>();
				result.enumData = enumEntries.empty() ? nullptr : enumEntries.data();
				result.enumCount = static_cast<std::uint32_t>(enumEntries.size());
			} else if constexpr (Type<T>::kind == TypeKind::Struct) {
				result.memberData = members.empty() ? nullptr : members.data();
				result.memberCount = static_cast<std::uint32_t>(members.size());
			} else if constexpr (Type<T>::kind == TypeKind::Array) {
				result.relatedTypeId = typeId<typename Type<T>::Element>();
				result.elementCount = static_cast<std::uint32_t>(Type<T>::count);
			}
			return result;
		}
	};

	template<class... Present>
	[[nodiscard]] static consteval auto makeDescriptors(detail::TypeList<Present...>) noexcept
	{
		return std::array<TypeDescriptor, sizeof...(Present)>{Stored<Present>::make()...};
	}

	inline static constexpr auto descriptors_ = makeDescriptors(Ordered{});

	[[nodiscard]] static consteval std::uint64_t totalBytes() noexcept
	{
		std::uint64_t result = 0;
		for (const auto& descriptor : descriptors_)
			result += descriptor.recordBytes;
		return result;
	}

	inline static constexpr std::uint64_t wideRecordsBytes_ = totalBytes();
	static_assert(wideRecordsBytes_ <= Limits::maxDescriptorBytes,
	              "Type records exceed descriptor byte ceiling");

	[[nodiscard]] static consteval std::uint64_t totalEnumEntries() noexcept
	{
		std::uint64_t result = 0;
		for (const auto& descriptor : descriptors_)
			result += descriptor.enumCount;
		return result;
	}

	static_assert(totalEnumEntries() <= Limits::maxEnumEntriesTotal,
	              "Enum dictionaries exceed model entry ceiling");

	// Wire type names in TypeId order; empty for an unnamed enum/struct and for
	// every array, scalar and Void. This nested class is outside TypeDescriptor
	// and the structured ABI tag, and only a Descriptor that emits names
	// instantiates it, so a build with TELEMETRY_TYPE_NAMES=0 compiles no name.
	struct Names {
		template<class... Present>
		[[nodiscard]] static consteval auto make(detail::TypeList<Present...>) noexcept
		{
			return std::array<std::string_view, sizeof...(Present)>{Type<Present>::name...};
		}

		inline static constexpr auto values = make(Ordered{});
	};

public:
	static constexpr std::uint32_t recordsBytes = static_cast<std::uint32_t>(wideRecordsBytes_);

	// The returned pointers refer to immutable static template storage and
	// remain valid independently of any runtime Model copy or destruction.
	[[nodiscard]] static constexpr TypeRegistryView view() noexcept
	{
		return {descriptors_.data(), typeCount, recordsBytes};
	}

	// Borrow the wire names indexed by TypeId; a view never outlives the
	// static array. TypeDescriptor::recordBytes always excludes the name.
	[[nodiscard]] static constexpr std::span<const std::string_view> typeNames() noexcept
	{
		return Names::values;
	}

	template<TypeId Id>
	[[nodiscard]] static constexpr const TypeDescriptor& descriptor() noexcept
	{
		static_assert(Id < typeCount, "TypeId is outside this TypeRegistry");
		if constexpr (Id < typeCount)
			return descriptors_[Id];
		else
			return descriptors_[0]; // Unreachable after the assertion.
	}
};

namespace detail {

// Expands an ordered type sequence into a TypeRegistry.
template<class... T>
struct RegistryFromList<TypeList<T...>> {
	using type = TypeRegistry<T...>;
};

} // namespace detail

} // namespace telemetry

#endif

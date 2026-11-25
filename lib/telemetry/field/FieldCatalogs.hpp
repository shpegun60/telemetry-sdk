/*
 * @file FieldCatalogs.hpp
 * @brief Field groups, native routing and positional encoded access.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Groups stable local Field tables for typed access and packed-ID dispatch.
 *
 * The runtime index borrows catalog rows and selects a group and local entry
 * without flattening values or searching names. Typed readAs/writeAs retains
 * checked conversion rules across groups. Catalog tables and their referenced
 * local tables must remain alive at stable addresses.
 */

#ifndef TELEMETRY_FIELD_FIELD_CATALOGS_HPP
#define TELEMETRY_FIELD_FIELD_CATALOGS_HPP
#pragma once

#include "../model/Catalog.hpp"
#include "FieldTable.hpp"

namespace telemetry {

// One named borrowed runtime group. For a nonempty group entries addresses
// count valid rows; neither the row array nor the name is owned by this view.
struct FieldCatalog {
	const char* name;
	const FieldEntry* entries;
	std::uint32_t count;
};

// Copyable runtime view over coherent catalog storage. The constructor
// assumes count describes valid catalog rows; find() checks the requested ID,
// not the integrity of user-forged catalog data. All backing tables and names
// must remain alive, and callers serialize access to their endpoint owners.
// Public methods:
// - FieldIndex(): Borrow Field catalogs.
// - catalogs(): Borrow catalog rows.
// - count(): Count catalog groups.
// - forEachEntry(): Visit erased entries.
// - forEachEntryWhile(): Visit erased entries until false.
// - find(): Find checked ID.
// - readEncoded(): Read encoded value.
// - writeEncoded(): Apply encoded value.
class FieldIndex {
public:
	constexpr FieldIndex(const FieldCatalog* catalogs, std::uint32_t count) noexcept
	    : catalogs_(catalogs), count_(count)
	{}

	[[nodiscard]] constexpr const FieldCatalog* catalogs() const noexcept
	{
		return catalogs_;
	}

	[[nodiscard]] constexpr std::uint32_t count() const noexcept
	{
		return count_;
	}

	template<class Visitor>
	constexpr void forEachEntry(Visitor&& visitor) const
	{
		detail::forEachCatalogEntry(catalogs_, count_, visitor);
	}

	template<class Visitor>
	[[nodiscard]] constexpr bool forEachEntryWhile(Visitor&& visitor) const
	{
		return detail::forEachCatalogEntryWhile(catalogs_, count_, visitor);
	}

	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] constexpr const FieldEntry* find(Id id) const noexcept
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return nullptr;
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		const auto entry = packed & 0xffffu;
		if (group >= count_)
			return nullptr;
		const auto& catalog = catalogs_[group];
		return entry < catalog.count ? catalog.entries + entry : nullptr;
	}

	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] EncodedReadResult readEncoded(Id id, std::span<std::byte> output,
	                                            Workspace& workspace) const noexcept
	{
		const auto* entry = find(id);
		if (entry == nullptr)
			return {DispatchStatus::NotFound, 0};
		return entry->readEncoded(output, workspace);
	}

	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] EncodedWriteResult writeEncoded(Id id, std::span<const std::byte> input,
	                                              Workspace& workspace) const noexcept
	{
		const auto* entry = find(id);
		if (entry == nullptr)
			return {DispatchStatus::NotFound};
		return entry->writeEncoded(input, workspace);
	}

	// Public layout is recorded by the compiled adapter's exact ABI tag.
	const FieldCatalog* catalogs_;
	std::uint32_t count_;
};

namespace model_detail {
// Recognizes local FieldTable groups at compile time.
template<class T>
struct IsFieldTable : std::false_type {};

// Recognizes local FieldTable groups at compile time.
template<class... D>
struct IsFieldTable<FieldTable<D...>> : std::true_type {};
} // namespace model_detail

namespace detail {

// Owns catalog rows while borrowing stable local tables through its groups.
// Declaration order supplies packed group positions. The catalog cannot move
// or copy because runtime indexes borrow its row array; typed access continues
// through each group's original table without owning values.
// Public methods:
// - FieldCatalogTable(): Own Field catalogs.
// - index(): Borrow runtime index.
// - data(): Borrow catalog rows.
// - size(): Count declared groups.
// - empty(): Check catalog emptiness.
// - begin(): Borrow first group.
// - end(): Borrow end position.
// - operator[](): Borrow unchecked group.
// - forEach(): Visit typed definitions.
// - forEachWhile(): Visit until false.
// - forEachEntry(): Visit erased entries.
// - forEachEntryWhile(): Visit erased entries until false.
// - visit(): Select checked ID.
// - readAs(): Read owning conversion.
// - readBorrowed(): Borrow exact native value.
// - readAsResult(): Read owning conversion with native failure status.
// - writeAs(): Apply checked conversion.
template<class... Groups>
class FieldCatalogTable {
	static_assert(sizeof...(Groups) <= idComponentCapacity,
	              "Field catalog exceeds the 16-bit group position space");
	static_assert((model_detail::IsFieldTable<typename Groups::TableType>::value && ...),
	              "Field catalog groups require FieldTable instances");

public:
	// Positional roots retain every field across groups, including duplicates.
	using RootTypes = typename detail::ConcatLists<typename Groups::TableType::RootTypes...>::type;
	using RegistryRootTypes = typename detail::UniqueFirst<
	    typename detail::ConcatLists<typename Groups::TableType::RegistryRootTypes...>::type>::type;
	static constexpr std::size_t staticSize = sizeof...(Groups);

	constexpr explicit FieldCatalogTable(Groups... groups) noexcept
	    : groups_(groups...), catalogs_(makeCatalogs(std::index_sequence_for<Groups...>{}))
	{}

	FieldCatalogTable(const FieldCatalogTable&) = delete;
	FieldCatalogTable& operator=(const FieldCatalogTable&) = delete;
	FieldCatalogTable(FieldCatalogTable&&) = delete;
	FieldCatalogTable& operator=(FieldCatalogTable&&) = delete;

	[[nodiscard]] constexpr FieldIndex index() const& noexcept
	{
		return {catalogs_.data(), static_cast<std::uint32_t>(staticSize)};
	}

	FieldIndex index() const&& = delete;

	[[nodiscard]] constexpr const FieldCatalog* data() const& noexcept
	{
		return catalogs_.data();
	}

	const FieldCatalog* data() const&& = delete;

	[[nodiscard]] constexpr std::size_t size() const noexcept
	{
		return staticSize;
	}

	[[nodiscard]] constexpr bool empty() const noexcept
	{
		return staticSize == 0;
	}

	[[nodiscard]] constexpr const FieldCatalog* begin() const& noexcept
	{
		return catalogs_.data();
	}

	const FieldCatalog* begin() const&& = delete;

	[[nodiscard]] constexpr const FieldCatalog* end() const& noexcept
	{
		// std::array may expose checked iterators on other standard libraries.
		// Our view uses pointers and never adds zero to a possibly null data().
		if constexpr (staticSize == 0)
			return catalogs_.data();
		else
			return catalogs_.data() + staticSize;
	}

	const FieldCatalog* end() const&& = delete;

	// Unchecked group access requires i < size(); packed-ID APIs check both
	// group and entry positions before returning a row or calling a visitor.
	[[nodiscard]] constexpr const FieldCatalog& operator[](std::size_t i) const& noexcept
	{
		return catalogs_[i];
	}

	const FieldCatalog& operator[](std::size_t) const&& = delete;

	// Traverse declaration order with the original borrowed definitions.
	// visit() below reports selection only; callback results are discarded.
	template<class Visitor>
	constexpr void forEach(Visitor&& visitor) const&
	{
		detail::forEachGroup(groups_, visitor, std::index_sequence_for<Groups...>{});
	}

	template<class Visitor>
	void forEach(Visitor&&) const&& = delete;

	template<class Visitor>
	[[nodiscard]] constexpr bool forEachWhile(Visitor&& visitor) const&
	{
		return detail::forEachGroupWhile(groups_, visitor, std::index_sequence_for<Groups...>{});
	}

	template<class Visitor>
	bool forEachWhile(Visitor&&) const&& = delete;

	template<class Visitor>
	constexpr void forEachEntry(Visitor&& visitor) const&
	{
		index().forEachEntry(visitor);
	}

	template<class Visitor>
	void forEachEntry(Visitor&&) const&& = delete;

	template<class Visitor>
	[[nodiscard]] constexpr bool forEachEntryWhile(Visitor&& visitor) const&
	{
		return index().forEachEntryWhile(visitor);
	}

	template<class Visitor>
	bool forEachEntryWhile(Visitor&&) const&& = delete;

	template<class... Explicit, std::integral Id, class Visitor>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] bool visit(Id id, Visitor&& visitor) const&
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return false;
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return false;
		using Dispatch =
		    detail::GroupDispatch<std::tuple<Groups...>, std::remove_reference_t<Visitor>>;
		return Dispatch::entries[group](groups_, packed & 0xffffu, visitor);
	}

	template<class... Explicit, std::integral Id, class Visitor>
	    requires(sizeof...(Explicit) == 0)
	bool visit(Id, Visitor&&) const&& = delete;

	// Packed runtime IDs are checked at their original width, as in index().
	// A structural mismatch returns nullopt/InvalidValue without a callback.
	template<class To, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0 && std::is_same_v<To, std::remove_cvref_t<To>>)
	[[nodiscard]] std::optional<To> readAs(Id id) const& noexcept
	{
		static_assert(Type<To>::kind != TypeKind::Void,
		              "Field readAs requires a native value type");
		if (!telemetry::detail::indexFits<PackedId>(id))
			return std::nullopt;
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return std::nullopt;
		return detail::FieldGroupAccessDispatch<std::tuple<Groups...>, To>::reads[group](
		    groups_, packed & 0xffffu);
	}

	template<class To, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	void readAs(Id) const&& = delete;

	template<class To, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0 && std::is_same_v<To, std::remove_cvref_t<To>>)
	[[nodiscard]] BorrowedValue<To> readBorrowed(Id id) const& noexcept
	{
		static_assert(Type<To>::kind != TypeKind::Void,
		              "Field readBorrowed requires a native value type");
		if (!telemetry::detail::indexFits<PackedId>(id))
			return {};
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return {};
		return detail::FieldGroupReadDispatch<std::tuple<Groups...>, To>::borrows[group](
		    groups_, packed & 0xffffu);
	}

	template<class To, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	void readBorrowed(Id) const&& = delete;

	// Failure precedence is checked ID, declared shape, getter availability,
	// then checked conversion. No failure path fabricates a value of To.
	template<class To, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0 && std::is_same_v<To, std::remove_cvref_t<To>>)
	[[nodiscard]] FieldReadResult<To> readAsResult(Id id) const& noexcept
	{
		static_assert(Type<To>::kind != TypeKind::Void,
		              "Field readAsResult requires a native value type");
		if (!telemetry::detail::indexFits<PackedId>(id))
			return FieldReadResult<To>::failure(FieldReadStatus::NotFound);
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return FieldReadResult<To>::failure(FieldReadStatus::NotFound);
		return detail::FieldGroupReadDispatch<std::tuple<Groups...>, To>::reads[group](
		    groups_, packed & 0xffffu);
	}

	template<class To, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	void readAsResult(Id) const&& = delete;

	template<class... Explicit, std::integral Id, class From>
	    requires(sizeof...(Explicit) == 0 && !std::is_volatile_v<From>)
	[[nodiscard]] telemetry::WriteResult writeAs(Id id, const From& value) const& noexcept
	{
		static_assert(Type<From>::kind != TypeKind::Void,
		              "Field writeAs requires a native value type");
		if (!telemetry::detail::indexFits<PackedId>(id))
			return telemetry::WriteResult::NotFound;
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return telemetry::WriteResult::NotFound;
		return detail::FieldGroupAccessDispatch<std::tuple<Groups...>, From>::writes[group](
		    groups_, packed & 0xffffu, value);
	}

	template<class... Explicit, std::integral Id, class From>
	    requires(sizeof...(Explicit) == 0)
	void writeAs(Id, const From&) const&& = delete;

	// Static endpoint TypeId metadata against the complete Model registry.
	template<class Registry>
	struct TypeStorage {
		inline static constexpr std::array<ValueTypeCatalog, staticSize> catalogs{
		    {ValueTypeCatalog{Groups::TableType::template TypeStorage<Registry>::entries.data(),
		                      static_cast<std::uint32_t>(Groups::TableType::staticSize)}...}};
	};

private:
	friend struct CatalogAccess;

	// Compile-time typed access by packed ID, not a local position enum: the
	// engine of paths and handles, reached through CatalogAccess.
	template<auto Id>
	[[nodiscard]] constexpr decltype(auto) get() const& noexcept
	{
		constexpr auto packed = telemetry::detail::packedIdValue<Id>();
		constexpr auto group = packed >> 16;
		constexpr auto entry = packed & 0xffffu;
		static_assert(group < staticSize, "Field group is outside this catalog");
		if constexpr (group < staticSize)
			return std::get<group>(groups_).table->template get<entry>();
	}

	template<auto Id>
	void get() const&& = delete;

	template<auto Id>
	[[nodiscard]] auto read() const noexcept
	{
		constexpr auto packed = telemetry::detail::packedIdValue<Id>();
		constexpr auto group = packed >> 16;
		constexpr auto entry = packed & 0xffffu;
		static_assert(group < staticSize, "Field group is outside this catalog");
		if constexpr (group < staticSize)
			return std::get<group>(groups_).table->template read<entry>();
	}

	template<auto Id, class Argument>
	[[nodiscard]] telemetry::WriteResult write(Argument&& value) const noexcept
	{
		constexpr auto packed = telemetry::detail::packedIdValue<Id>();
		constexpr auto group = packed >> 16;
		constexpr auto entry = packed & 0xffffu;
		static_assert(group < staticSize, "Field group is outside this catalog");
		if constexpr (group < staticSize)
			return std::get<group>(groups_).table->template write<entry>(
			    std::forward<Argument>(value));
	}

	template<class To, auto Id>
	    requires std::is_same_v<To, std::remove_cvref_t<To>>
	[[nodiscard]] std::optional<To> readAs() const& noexcept
	{
		return get<Id>().template readAs<To>();
	}

	template<class To, auto Id>
	void readAs() const&& = delete;

	template<class To, auto Id>
	    requires std::is_same_v<To, std::remove_cvref_t<To>>
	[[nodiscard]] BorrowedValue<To> readBorrowed() const& noexcept
	{
		const auto& definition = get<Id>();
		using Definition = std::remove_cvref_t<decltype(definition)>;
		static_assert(std::is_same_v<To, typename Definition::Value> && Definition::borrowsValue,
		              "Field readBorrowed requires the exact borrowed value type");
		return detail::readFieldBorrowed<To>(definition);
	}

	template<class To, auto Id>
	void readBorrowed() const&& = delete;

	template<class To, auto Id>
	    requires std::is_same_v<To, std::remove_cvref_t<To>>
	[[nodiscard]] FieldReadResult<To> readAsResult() const& noexcept
	{
		return get<Id>().template readAsResult<To>();
	}

	template<class To, auto Id>
	void readAsResult() const&& = delete;

	template<auto Id, class From>
	    requires(!std::is_volatile_v<From>)
	[[nodiscard]] telemetry::WriteResult writeAs(const From& value) const& noexcept
	{
		return get<Id>().writeAs(value);
	}

	template<auto Id, class From>
	void writeAs(const From&) const&& = delete;

	template<std::size_t... I>
	constexpr auto makeCatalogs(std::index_sequence<I...>) noexcept
	{
		return std::array<FieldCatalog, staticSize>{
		    {FieldCatalog{std::get<I>(groups_).name, std::get<I>(groups_).table->data(),
		                  static_cast<std::uint32_t>(std::get<I>(groups_).table->size())}...}};
	}

	std::tuple<Groups...> groups_;
	std::array<FieldCatalog, staticSize> catalogs_;
};

template<class... Groups>
FieldCatalogTable(Groups...) -> FieldCatalogTable<Groups...>;

} // namespace detail

} // namespace telemetry

#endif

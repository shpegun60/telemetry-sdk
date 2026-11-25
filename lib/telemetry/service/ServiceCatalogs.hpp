/*
 * @file ServiceCatalogs.hpp
 * @brief Borrowed Service groups, compile-time routing and O(1) ID lookup.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Groups stable local Service tables for typed and runtime request/response calls.
 *
 * Packed identifiers select a catalog and local entry in constant time. The
 * runtime index borrows erased rows while type catalogs reference the complete
 * Model registry. None of these views owns owners or payloads; catalog tables
 * and all referenced local tables must stay alive at stable addresses.
 */

#ifndef TELEMETRY_SERVICE_SERVICE_CATALOGS_HPP
#define TELEMETRY_SERVICE_SERVICE_CATALOGS_HPP
#pragma once

#include "ServiceTable.hpp"
#include "../model/Catalog.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace telemetry {

namespace model_detail {

// Recognizes local ServiceTable groups at compile time.
template<class Table>
struct IsServiceTable : std::false_type {};

// Recognizes local ServiceTable groups at compile time.
template<class... Definitions>
struct IsServiceTable<ServiceTable<Definitions...>> : std::true_type {};

} // namespace model_detail

// One named borrowed group of erased Service rows. Name and count-row storage
// stay valid for all runtime indexes; this view does not extend their lifetimes.
struct ServiceCatalog {
	const char* name;
	const ServiceEntry* entries;
	std::uint32_t count;
};

// Parallel immutable request/response TypeId pairs for one Service catalog.
// Their positions match ServiceCatalog rows in the complete Model registry.
struct ServiceTypeCatalog {
	const ServiceTypePair* entries;
	std::uint32_t count;
};

// Copyable runtime index over coherent catalog storage supplied by the caller.
// Incoming packed IDs are checked before narrowing and row selection. Catalog
// contents themselves are a construction precondition; find() cannot validate
// forged pointers or extend local-table and owner lifetimes.
// Public methods:
// - ServiceIndex(): Borrow Service catalogs.
// - catalogs(): Borrow catalog rows.
// - count(): Count catalog groups.
// - forEachEntry(): Visit erased entries.
// - forEachEntryWhile(): Visit erased entries until false.
// - find(): Find checked ID.
// - callEncoded(): Invoke encoded service.
class ServiceIndex {
public:
	constexpr ServiceIndex(const ServiceCatalog* catalogs, std::uint32_t count) noexcept
	    : catalogs_(catalogs), count_(count)
	{}

	[[nodiscard]] constexpr const ServiceCatalog* catalogs() const noexcept
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
	[[nodiscard]] constexpr const ServiceEntry* find(Id id) const noexcept
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return nullptr;
		const auto packed = static_cast<PackedId>(id);
		const auto groupPosition = static_cast<std::uint32_t>(packed >> 16);
		const auto entryPosition = static_cast<std::uint32_t>(packed & 0xffffu);
		if (groupPosition >= count_)
			return nullptr;
		const auto& catalog = catalogs_[groupPosition];
		return entryPosition < catalog.count ? catalog.entries + entryPosition : nullptr;
	}

	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] EncodedCallResult callEncoded(Id id, std::span<const std::byte> input,
	                                            std::span<std::byte> output,
	                                            Workspace& workspace) const noexcept
	{
		const ServiceEntry* entry = find(id);
		if (entry == nullptr)
			return {DispatchStatus::NotFound, ServiceStatus::Ok, 0};
		return entry->callEncoded(input, output, workspace);
	}

public:
	// This is a borrowed runtime view. The members are public so the exact
	// cross-translation-unit ABI tag can cover both offsets.
	const ServiceCatalog* catalogs_;
	std::uint32_t count_;
};

namespace detail {

// Owns runtime catalog rows while borrowing original stable Service tables.
// Group declaration order defines global positions. Typed access and visitation
// retain exact definitions; erased indexes borrow the generated row array,
// requiring this catalog table to remain at its original address.
// Public methods:
// - ServiceCatalogTable(): Own Service catalogs.
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
// - call(): Invoke native service.
// - callAs(): Select runtime owning response or exact result.
// - callBorrowed(): Select runtime borrowed response.
template<class... Groups>
class ServiceCatalogTable {
	static_assert(sizeof...(Groups) <= idComponentCapacity,
	              "Service catalog exceeds the 16-bit group position space");
	static_assert((model_detail::IsServiceTable<typename Groups::TableType>::value && ...),
	              "Service catalog groups require ServiceTable instances");

public:
	// Positional request/response pairs retain every service across groups.
	using RootTypes = typename detail::ConcatLists<typename Groups::TableType::RootTypes...>::type;
	using RegistryRootTypes = typename detail::UniqueFirst<
	    typename detail::ConcatLists<typename Groups::TableType::RegistryRootTypes...>::type>::type;
	static constexpr std::size_t staticSize = sizeof...(Groups);

	constexpr explicit ServiceCatalogTable(Groups... groups) noexcept
	    : groups_(groups...), catalogs_(makeCatalogs(std::index_sequence_for<Groups...>{}))
	{}

	ServiceCatalogTable(const ServiceCatalogTable&) = delete;
	ServiceCatalogTable& operator=(const ServiceCatalogTable&) = delete;
	ServiceCatalogTable(ServiceCatalogTable&&) = delete;
	ServiceCatalogTable& operator=(ServiceCatalogTable&&) = delete;

	[[nodiscard]] constexpr ServiceIndex index() const& noexcept
	{
		return {catalogs_.data(), static_cast<std::uint32_t>(staticSize)};
	}

	ServiceIndex index() const&& = delete;

	[[nodiscard]] constexpr const ServiceCatalog* data() const& noexcept
	{
		return catalogs_.data();
	}

	const ServiceCatalog* data() const&& = delete;

	[[nodiscard]] constexpr std::size_t size() const noexcept
	{
		return staticSize;
	}

	[[nodiscard]] constexpr bool empty() const noexcept
	{
		return staticSize == 0;
	}

	[[nodiscard]] constexpr const ServiceCatalog* begin() const& noexcept
	{
		return catalogs_.data();
	}

	const ServiceCatalog* begin() const&& = delete;

	[[nodiscard]] constexpr const ServiceCatalog* end() const& noexcept
	{
		// std::array may expose checked iterators on other standard libraries.
		// Our view uses pointers and never adds zero to a possibly null data().
		if constexpr (staticSize == 0)
			return catalogs_.data();
		else
			return catalogs_.data() + staticSize;
	}

	const ServiceCatalog* end() const&& = delete;

	// Unchecked group access requires i < size(); find()/visit() are the checked
	// interfaces for packed IDs received from external callers.
	[[nodiscard]] constexpr const ServiceCatalog& operator[](std::size_t i) const& noexcept
	{
		return catalogs_[i];
	}

	const ServiceCatalog& operator[](std::size_t) const&& = delete;

	// Traverse borrowed native declarations without creating requests/results.
	// visit() reports selection only, and callback return values are discarded.
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

	// Flat access reuses the checked packed-ID route and exact native result.
	template<class Response, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0 && detail::flatServiceResponse<Response>)
	[[nodiscard]] ServiceCallResult<Response> callAs(Id id) const& noexcept
	{
		return ServiceCallResult<Response>::fromNative([&]() noexcept {
			return this->template callAs<ServiceResult<Response>>(id);
		});
	}

	template<class Response, class... Explicit, std::integral Id, class Argument>
	    requires(sizeof...(Explicit) == 0 && detail::flatServiceResponse<Response> &&
	             !std::is_volatile_v<Argument>)
	[[nodiscard]] ServiceCallResult<Response> callAs(Id id, const Argument& request) const& noexcept
	{
		return ServiceCallResult<Response>::fromNative([&]() noexcept {
			return this->template callAs<ServiceResult<Response>>(id, request);
		});
	}

	template<class Response, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0 && detail::flatServiceResponse<Response> &&
	             !std::is_void_v<Response>)
	[[nodiscard]] BorrowedServiceCallResult<Response> callBorrowed(Id id) const& noexcept
	{
		return BorrowedServiceCallResult<Response>::fromNative([&]() noexcept {
			return this->template callAs<BorrowedServiceResult<Response>>(id);
		});
	}

	template<class Response, class... Explicit, std::integral Id, class Argument>
	    requires(sizeof...(Explicit) == 0 && detail::flatServiceResponse<Response> &&
	             !std::is_void_v<Response> && !std::is_volatile_v<Argument>)
	[[nodiscard]] BorrowedServiceCallResult<Response>
	callBorrowed(Id id, const Argument& request) const& noexcept
	{
		return BorrowedServiceCallResult<Response>::fromNative([&]() noexcept {
			return this->template callAs<BorrowedServiceResult<Response>>(id, request);
		});
	}

	template<class Response, class... Explicit, std::integral Id, class... Args>
	    requires(sizeof...(Explicit) == 0 && detail::flatServiceResponse<Response> &&
	             !std::is_void_v<Response>)
	void callBorrowed(Id, Args&&...) const&& = delete;

	// Keep packed-ID validation before narrowing, then index group and row.
	template<class Result, class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0 && detail::nativeServiceResult<Result>)
	[[nodiscard]] NativeCallResult<Result> callAs(Id id) const& noexcept
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return NativeCallResult<Result>::failure(NativeCallStatus::NotFound);
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return NativeCallResult<Result>::failure(NativeCallStatus::NotFound);
		using Dispatch = detail::NativeGroupCallDispatch<std::tuple<Groups...>, Result, void>;
		return Dispatch::entries[group](groups_, packed & 0xffffu, nullptr);
	}

	template<class Result, class... Explicit, std::integral Id, class Argument>
	    requires(sizeof...(Explicit) == 0 && detail::nativeServiceResult<Result> &&
	             !std::is_volatile_v<Argument>)
	[[nodiscard]] NativeCallResult<Result> callAs(Id id, const Argument& request) const& noexcept
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return NativeCallResult<Result>::failure(NativeCallStatus::NotFound);
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return NativeCallResult<Result>::failure(NativeCallStatus::NotFound);
		using Dispatch = detail::NativeGroupCallDispatch<std::tuple<Groups...>, Result,
		                                                 std::remove_cvref_t<Argument>>;
		return Dispatch::entries[group](groups_, packed & 0xffffu, request);
	}

	template<class Result, class... Explicit, std::integral Id, class... Args>
	    requires(sizeof...(Explicit) == 0 &&
	             (detail::nativeServiceResult<Result> || detail::flatServiceResponse<Result>))
	void callAs(Id, Args&&...) const&& = delete;

	// Static endpoint TypeId metadata against the complete Model registry.
	template<class Registry>
	struct TypeStorage {
		inline static constexpr std::array<ServiceTypeCatalog, staticSize> catalogs{
		    {ServiceTypeCatalog{Groups::TableType::template TypeStorage<Registry>::entries.data(),
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
		static_assert(group < staticSize, "Service group is outside this catalog");
		if constexpr (group < staticSize)
			return std::get<group>(groups_).table->template get<entry>();
	}

	template<auto Id>
	void get() const&& = delete;

	template<auto Id, class... Args>
	[[nodiscard]] decltype(auto) call(Args&&... args) const noexcept
	{
		constexpr PackedId packed = telemetry::detail::packedIdValue<Id>();
		constexpr std::size_t groupPosition = packed >> 16;
		constexpr std::size_t entryPosition = packed & 0xffffu;
		static_assert(groupPosition < staticSize, "Service group is outside this catalog");
		if constexpr (groupPosition < staticSize) {
			return std::get<groupPosition>(groups_).table->template call<entryPosition>(
			    std::forward<Args>(args)...);
		}
	}

	template<std::size_t... I>
	[[nodiscard]] constexpr std::array<ServiceCatalog, staticSize>
	makeCatalogs(std::index_sequence<I...>) noexcept
	{
		return {
		    {ServiceCatalog{std::get<I>(groups_).name, std::get<I>(groups_).table->data(),
		                    static_cast<std::uint32_t>(std::get<I>(groups_).table->size())}...}};
	}

	std::tuple<Groups...> groups_;
	std::array<ServiceCatalog, staticSize> catalogs_;
};

template<class... Groups>
ServiceCatalogTable(Groups...) -> ServiceCatalogTable<Groups...>;

} // namespace detail

} // namespace telemetry

#endif

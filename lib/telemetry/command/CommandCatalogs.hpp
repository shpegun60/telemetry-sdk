/*
 * @file CommandCatalogs.hpp
 * @brief Command groups, native routing and positional encoded execution.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Groups stable local Command tables into one positional identifier space.
 *
 * Catalog tables preserve typed access while CommandIndex borrows only the
 * runtime rows needed by encoded dispatch. Packed identifiers select a group
 * and then an entry without a name search. Catalogs and every referenced local
 * table must remain alive at stable addresses.
 */

#ifndef TELEMETRY_COMMAND_COMMAND_CATALOGS_HPP
#define TELEMETRY_COMMAND_COMMAND_CATALOGS_HPP
#pragma once

#include "../model/Catalog.hpp"
#include "CommandTable.hpp"

namespace telemetry {

// One named borrowed runtime group. The name and count-row entry array must
// remain valid for every index and ModelView referring to this catalog.
struct CommandCatalog {
	const char* name;
	const CommandEntry* entries;
	std::uint32_t count;
};

// Copyable runtime view over a coherent borrowed catalog array. find() checks
// the incoming packed ID at its original width and then both positions. It
// assumes the supplied catalog rows are internally valid, and owns no request,
// callback or transport state.
// Public methods:
// - CommandIndex(): Borrow Command catalogs.
// - catalogs(): Borrow catalog rows.
// - count(): Count catalog groups.
// - forEachEntry(): Visit erased entries.
// - forEachEntryWhile(): Visit erased entries until false.
// - find(): Find checked ID.
// - executeEncoded(): Execute encoded request.
class CommandIndex {
public:
	constexpr CommandIndex(const CommandCatalog* catalogs, std::uint32_t count) noexcept
	    : catalogs_(catalogs), count_(count)
	{}

	[[nodiscard]] constexpr const CommandCatalog* catalogs() const noexcept
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
	[[nodiscard]] constexpr const CommandEntry* find(Id id) const noexcept
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
	[[nodiscard]] EncodedCommandResult executeEncoded(Id id, std::span<const std::byte> input,
	                                                  Workspace& workspace) const noexcept
	{
		const auto* entry = find(id);
		if (entry == nullptr)
			return {DispatchStatus::NotFound};
		return entry->executeEncoded(input, workspace);
	}

	const CommandCatalog* catalogs_;
	std::uint32_t count_;
};

namespace model_detail {
// Recognizes local CommandTable groups at compile time.
template<class T>
struct IsCommandTable : std::false_type {};

// Recognizes local CommandTable groups at compile time.
template<class... D>
struct IsCommandTable<CommandTable<D...>> : std::true_type {};
} // namespace model_detail

namespace detail {

// Owns runtime catalog rows and borrows the original nonmoving Command tables.
// Group declaration order defines global IDs. Typed access and traversal retain
// each exact binding; runtime indexes borrow this table's row array, so this
// catalog table also has a fixed address and cannot move or copy.
// Public methods:
// - CommandCatalogTable(): Own Command catalogs.
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
// - call(): Invoke native command.
// - callAs(): Select runtime signature.
template<class... Groups>
class CommandCatalogTable {
	static_assert(sizeof...(Groups) <= idComponentCapacity,
	              "Command catalog exceeds the 16-bit group position space");
	static_assert((model_detail::IsCommandTable<typename Groups::TableType>::value && ...),
	              "Command catalog groups require CommandTable instances");

public:
	// Positional request roots retain every command across groups.
	using RootTypes = typename detail::ConcatLists<typename Groups::TableType::RootTypes...>::type;
	using RegistryRootTypes = typename detail::UniqueFirst<
	    typename detail::ConcatLists<typename Groups::TableType::RegistryRootTypes...>::type>::type;
	static constexpr std::size_t staticSize = sizeof...(Groups);

	constexpr explicit CommandCatalogTable(Groups... groups) noexcept
	    : groups_(groups...), catalogs_(makeCatalogs(std::index_sequence_for<Groups...>{}))
	{}

	CommandCatalogTable(const CommandCatalogTable&) = delete;
	CommandCatalogTable& operator=(const CommandCatalogTable&) = delete;
	CommandCatalogTable(CommandCatalogTable&&) = delete;
	CommandCatalogTable& operator=(CommandCatalogTable&&) = delete;

	[[nodiscard]] constexpr CommandIndex index() const& noexcept
	{
		return {catalogs_.data(), static_cast<std::uint32_t>(staticSize)};
	}

	CommandIndex index() const&& = delete;

	[[nodiscard]] constexpr const CommandCatalog* data() const& noexcept
	{
		return catalogs_.data();
	}

	const CommandCatalog* data() const&& = delete;

	[[nodiscard]] constexpr std::size_t size() const noexcept
	{
		return staticSize;
	}

	[[nodiscard]] constexpr bool empty() const noexcept
	{
		return staticSize == 0;
	}

	[[nodiscard]] constexpr const CommandCatalog* begin() const& noexcept
	{
		return catalogs_.data();
	}

	const CommandCatalog* begin() const&& = delete;

	[[nodiscard]] constexpr const CommandCatalog* end() const& noexcept
	{
		// std::array may expose checked iterators on other standard libraries.
		// Our view uses pointers and never adds zero to a possibly null data().
		if constexpr (staticSize == 0)
			return catalogs_.data();
		else
			return catalogs_.data() + staticSize;
	}

	const CommandCatalog* end() const&& = delete;

	// Unchecked group access requires i < size(). Use packed-ID access for
	// fallible external positions so both group and local bounds are checked.
	[[nodiscard]] constexpr const CommandCatalog& operator[](std::size_t i) const& noexcept
	{
		return catalogs_[i];
	}

	const CommandCatalog& operator[](std::size_t) const&& = delete;

	// Visit original definitions in group/entry declaration order. Runtime
	// visit() reports selection, and neither form interprets callback results.
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

	// Keep packed-ID validation before narrowing, then index group and row.
	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] NativeCallResult<telemetry::CommandResult> callAs(Id id) const& noexcept
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return NativeCallResult<telemetry::CommandResult>::failure(NativeCallStatus::NotFound);
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return NativeCallResult<telemetry::CommandResult>::failure(NativeCallStatus::NotFound);
		using Dispatch =
		    detail::NativeGroupCallDispatch<std::tuple<Groups...>, telemetry::CommandResult, void>;
		return Dispatch::entries[group](groups_, packed & 0xffffu, nullptr);
	}

	template<class... Explicit, std::integral Id, class Argument>
	    requires(sizeof...(Explicit) == 0 && !std::is_volatile_v<Argument>)
	[[nodiscard]] NativeCallResult<telemetry::CommandResult>
	callAs(Id id, const Argument& request) const& noexcept
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return NativeCallResult<telemetry::CommandResult>::failure(NativeCallStatus::NotFound);
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		if (group >= staticSize)
			return NativeCallResult<telemetry::CommandResult>::failure(NativeCallStatus::NotFound);
		using Dispatch =
		    detail::NativeGroupCallDispatch<std::tuple<Groups...>, telemetry::CommandResult,
		                                    std::remove_cvref_t<Argument>>;
		return Dispatch::entries[group](groups_, packed & 0xffffu, request);
	}

	template<class... Explicit, std::integral Id, class... Args>
	    requires(sizeof...(Explicit) == 0)
	void callAs(Id, Args&&...) const&& = delete;

	// Runtime commands flatten the existing exact native dispatch outcome.
	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] CommandCallStatus call(Id id) const& noexcept
	{
		return result_detail::commandCallStatus(callAs(id));
	}

	template<class... Explicit, std::integral Id, class Argument>
	    requires(sizeof...(Explicit) == 0 && !std::is_volatile_v<Argument>)
	[[nodiscard]] CommandCallStatus call(Id id, const Argument& request) const& noexcept
	{
		return result_detail::commandCallStatus(callAs(id, request));
	}

	template<class... Explicit, std::integral Id, class... Args>
	    requires(sizeof...(Explicit) == 0)
	void call(Id, Args&&...) const&& = delete;

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
		static_assert(group < staticSize, "Command group is outside this catalog");
		if constexpr (group < staticSize)
			return std::get<group>(groups_).table->template get<entry>();
	}

	template<auto Id>
	void get() const&& = delete;

	template<auto Id, class... Args>
	[[nodiscard]] telemetry::CommandResult call(Args&&... args) const noexcept
	{
		constexpr auto packed = telemetry::detail::packedIdValue<Id>();
		constexpr auto group = packed >> 16;
		constexpr auto entry = packed & 0xffffu;
		static_assert(group < staticSize, "Command group is outside this catalog");
		if constexpr (group < staticSize)
			return std::get<group>(groups_).table->template call<entry>(
			    std::forward<Args>(args)...);
	}

	template<std::size_t... I>
	constexpr auto makeCatalogs(std::index_sequence<I...>) noexcept
	{
		return std::array<CommandCatalog, staticSize>{
		    {CommandCatalog{std::get<I>(groups_).name, std::get<I>(groups_).table->data(),
		                    static_cast<std::uint32_t>(std::get<I>(groups_).table->size())}...}};
	}

	std::tuple<Groups...> groups_;
	std::array<CommandCatalog, staticSize> catalogs_;
};

template<class... Groups>
CommandCatalogTable(Groups...) -> CommandCatalogTable<Groups...>;

} // namespace detail

} // namespace telemetry

#endif

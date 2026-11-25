/*
 * @file Model.hpp
 * @brief One type registry and runtime views for structured endpoint catalogs.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Combines endpoint catalogs with one structural type registry and runtime view.
 *
 * Registry roots follow Field, Command and Service declaration order; Service
 * requests precede their responses. The catalog engine borrows catalogs and
 * exposes their live entries alongside immutable metadata; the public Model is
 * the group tree built on it (GroupTree.hpp). Reported scratch maxima cover one
 * encoded operation, including alignment, rather than concurrent calls.
 */

#ifndef TELEMETRY_MODEL_MODEL_HPP
#define TELEMETRY_MODEL_MODEL_HPP
#pragma once

#include "../service/ServiceCatalogs.hpp"
#include "../field/FieldCatalogs.hpp"
#include "../command/CommandCatalogs.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>

namespace telemetry {

// Copyable erased view pairing runtime endpoint rows with structural metadata.
// Every pointer borrows coherent catalog/registry storage; copying the view does
// not extend any owner lifetime or freeze live values. Public aggregate members
// enable ABI inspection, but forged or inconsistent view contents are invalid.
// Public methods:
// - serviceTypeIds(): Find Service types.
// - fieldTypeId(): Find Field type.
// - commandTypeId(): Find Command type.
struct ModelView {
	TypeRegistryView types;
	ServiceIndex services;
	const ServiceTypeCatalog* serviceTypes;
	std::uint32_t serviceCatalogCount;
	FieldIndex fields{nullptr, 0};
	CommandIndex commands{nullptr, 0};
	const ValueTypeCatalog* fieldTypes = nullptr;
	std::uint32_t fieldCatalogCount = 0;
	const ValueTypeCatalog* commandTypes = nullptr;
	std::uint32_t commandCatalogCount = 0;

	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] constexpr std::optional<ServiceTypePair> serviceTypeIds(Id id) const noexcept
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return std::nullopt;
		const auto packed = static_cast<PackedId>(id);
		const auto groupPosition = static_cast<std::uint32_t>(packed >> 16);
		const auto entryPosition = static_cast<std::uint32_t>(packed & 0xffffu);
		if (groupPosition >= serviceCatalogCount)
			return std::nullopt;
		const auto& catalog = serviceTypes[groupPosition];
		if (entryPosition >= catalog.count)
			return std::nullopt;
		return catalog.entries[entryPosition];
	}

	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] constexpr std::optional<TypeId> fieldTypeId(Id id) const noexcept
	{
		return valueTypeId(fieldTypes, fieldCatalogCount, id);
	}

	template<class... Explicit, std::integral Id>
	    requires(sizeof...(Explicit) == 0)
	[[nodiscard]] constexpr std::optional<TypeId> commandTypeId(Id id) const noexcept
	{
		return valueTypeId(commandTypes, commandCatalogCount, id);
	}

private:
	template<std::integral Id>
	static constexpr std::optional<TypeId> valueTypeId(const ValueTypeCatalog* catalogs,
	                                                   std::uint32_t count, Id id) noexcept
	{
		if (!telemetry::detail::indexFits<PackedId>(id))
			return std::nullopt;
		const auto packed = static_cast<PackedId>(id);
		const auto group = packed >> 16;
		const auto entry = packed & 0xffffu;
		if (group >= count || entry >= catalogs[group].count)
			return std::nullopt;
		return catalogs[group].entries[entry];
	}
};

namespace detail {

// An empty family of the catalog engine, without a dummy local table.
struct EmptyEndpointCatalog {
	using RootTypes = detail::TypeList<>;
	using RegistryRootTypes = detail::TypeList<>;
};

inline constexpr EmptyEndpointCatalog emptyFields{};
inline constexpr EmptyEndpointCatalog emptyCommands{};

// The catalog engine under every group-tree Model, and the testing-only v3.0
// producer's base (telemetry/testing/V30Model.hpp); not a way to write a
// device. It borrows stable endpoint catalogs and derives one immutable
// registry from all roots. Copying it copies catalog pointers; it owns no live value storage.
// Catalogs, local tables, names and their transitive owner references must outlive
// any Model/index/provider use. Synchronization remains the application's job.
// Public methods:
// - CatalogModel(): Borrow endpoint catalogs.
// - types(): Borrow type registry.
// - typeId(): Resolve exact type.
// - serviceIndex(): Borrow Service index.
// - fieldIndex(): Borrow Field index.
// - commandIndex(): Borrow Command index.
// - view(): Borrow runtime view.
// - maxFieldScratch(): Bound Field scratch.
// - maxCommandScratch(): Bound Command scratch.
// - maxScratch(): Bound operation scratch.
// - maxFieldWireSize(): Bound Field payload.
// - maxCommandRequestWireSize(): Bound Command payload.
// - maxServiceRequestWireSize(): Bound request payload.
// - maxServiceScratch(): Bound Service scratch.
// - maxServiceResponseWireSize(): Bound response payload.
template<class Fields, class Commands, class Services>
class CatalogModel {
	using AllRoots =
	    typename detail::ConcatLists<typename detail::RegistryRootsOf<Fields>::type,
	                                 typename detail::RegistryRootsOf<Commands>::type,
	                                 typename detail::RegistryRootsOf<Services>::type>::type;

public:
	using Registry = typename detail::RegistryFromList<AllRoots>::type;

	// Deduce actual argument categories even for an explicitly named Model.
	// A fixed const-reference signature would accept braced temporary catalogs.
	template<class F, class C, class S>
	    requires(std::is_lvalue_reference_v<F> && std::is_lvalue_reference_v<C> &&
	             std::is_lvalue_reference_v<S> &&
	             std::is_same_v<std::remove_cvref_t<F>, std::remove_cv_t<Fields>> &&
	             std::is_same_v<std::remove_cvref_t<C>, std::remove_cv_t<Commands>> &&
	             std::is_same_v<std::remove_cvref_t<S>, std::remove_cv_t<Services>>)
	constexpr CatalogModel(F&& fields, C&& commands, S&& services) noexcept
	    : fields_(std::addressof(fields)), commands_(std::addressof(commands)),
	      services_(std::addressof(services))
	{}

	template<class F, class C, class S>
	    requires(!std::is_lvalue_reference_v<F> || !std::is_lvalue_reference_v<C> ||
	             !std::is_lvalue_reference_v<S>)
	CatalogModel(F&&, C&&, S&&) = delete;

	[[nodiscard]] static constexpr TypeRegistryView types() noexcept
	{
		return Registry::view();
	}

	template<class T>
	[[nodiscard]] static consteval TypeId typeId() noexcept
	{
		return Registry::template typeId<T>();
	}

	[[nodiscard]] constexpr ServiceIndex serviceIndex() const noexcept
	{
		return services_->index();
	}

	[[nodiscard]] constexpr FieldIndex fieldIndex() const noexcept
	{
		if constexpr (std::is_same_v<std::remove_cv_t<Fields>, EmptyEndpointCatalog>)
			return {nullptr, 0};
		else
			return fields_->index();
	}

	[[nodiscard]] constexpr CommandIndex commandIndex() const noexcept
	{
		if constexpr (std::is_same_v<std::remove_cv_t<Commands>, EmptyEndpointCatalog>)
			return {nullptr, 0};
		else
			return commands_->index();
	}

	// Expose matching runtime rows and TypeId catalogs from this exact Model.
	// The view contains no allocation and borrows the existing catalog arrays.
	[[nodiscard]] constexpr ModelView view() const noexcept
	{
		ModelView result{types(), serviceIndex(),
		                 Services::template TypeStorage<Registry>::catalogs.data(),
		                 static_cast<std::uint32_t>(services_->size())};
		result.fields = fieldIndex();
		result.commands = commandIndex();
		if constexpr (!std::is_same_v<std::remove_cv_t<Fields>, EmptyEndpointCatalog>) {
			result.fieldTypes = Fields::template TypeStorage<Registry>::catalogs.data();
			result.fieldCatalogCount = static_cast<std::uint32_t>(fields_->size());
		}
		if constexpr (!std::is_same_v<std::remove_cv_t<Commands>, EmptyEndpointCatalog>) {
			result.commandTypes = Commands::template TypeStorage<Registry>::catalogs.data();
			result.commandCatalogCount = static_cast<std::uint32_t>(commands_->size());
		}
		return result;
	}

	// Sufficient fresh-Workspace capacity for one operation in the selected
	// family, including worst-case initial alignment. Nested/concurrent calls
	// need all simultaneously live leases or separate Workspaces.
	[[nodiscard]] constexpr std::uint32_t maxFieldScratch() const noexcept
	{
		std::uint32_t result = 0;
		const auto index = fieldIndex();
		for (std::uint32_t group = 0; group < index.count(); ++group) {
			const auto& catalog = index.catalogs()[group];
			for (std::uint32_t entry = 0; entry < catalog.count; ++entry) {
				const auto& field = catalog.entries[entry];
				if (field.readScratchBytes > result)
					result = field.readScratchBytes;
				if (field.writeScratchBytes > result)
					result = field.writeScratchBytes;
			}
		}
		return result;
	}

	[[nodiscard]] constexpr std::uint32_t maxCommandScratch() const noexcept
	{
		return scratchFor(commandIndex());
	}

	[[nodiscard]] constexpr std::uint32_t maxScratch() const noexcept
	{
		auto result = maxServiceScratch();
		if (const auto bytes = maxFieldScratch(); bytes > result)
			result = bytes;
		if (const auto bytes = maxCommandScratch(); bytes > result)
			result = bytes;
		return result;
	}

	// Largest canonical payload extents, useful for caller-owned transport
	// buffers. These exclude packet framing and native object padding.
	[[nodiscard]] constexpr std::uint32_t maxFieldWireSize() const noexcept
	{
		std::uint32_t result = 0;
		const auto index = fieldIndex();
		for (std::uint32_t group = 0; group < index.count(); ++group) {
			const auto& catalog = index.catalogs()[group];
			for (std::uint32_t entry = 0; entry < catalog.count; ++entry)
				if (catalog.entries[entry].wireBytes > result)
					result = catalog.entries[entry].wireBytes;
		}
		return result;
	}

	[[nodiscard]] constexpr std::uint32_t maxCommandRequestWireSize() const noexcept
	{
		std::uint32_t result = 0;
		commandIndex().forEachEntry([&](PackedId, std::string_view, const CommandEntry& entry) {
			if (entry.requestWireBytes > result)
				result = entry.requestWireBytes;
		});
		return result;
	}

	[[nodiscard]] constexpr std::uint32_t maxServiceRequestWireSize() const noexcept
	{
		std::uint32_t result = 0;
		serviceIndex().forEachEntry([&](PackedId, std::string_view, const ServiceEntry& entry) {
			if (entry.requestWireBytes > result)
				result = entry.requestWireBytes;
		});
		return result;
	}

	[[nodiscard]] constexpr std::uint32_t maxServiceScratch() const noexcept
	{
		std::uint32_t result = 0;
		const auto index = serviceIndex();
		for (std::uint32_t group = 0; group < index.count(); ++group) {
			const auto& catalog = index.catalogs()[group];
			for (std::uint32_t entry = 0; entry < catalog.count; ++entry)
				if (catalog.entries[entry].scratchBytes > result)
					result = catalog.entries[entry].scratchBytes;
		}
		return result;
	}

	[[nodiscard]] constexpr std::uint32_t maxServiceResponseWireSize() const noexcept
	{
		std::uint32_t result = 0;
		const auto index = serviceIndex();
		for (std::uint32_t group = 0; group < index.count(); ++group) {
			const auto& catalog = index.catalogs()[group];
			for (std::uint32_t entry = 0; entry < catalog.count; ++entry)
				if (catalog.entries[entry].responseWireBytes > result)
					result = catalog.entries[entry].responseWireBytes;
		}
		return result;
	}

private:
	template<class Index>
	static constexpr std::uint32_t scratchFor(Index index) noexcept
	{
		std::uint32_t result = 0;
		for (std::uint32_t group = 0; group < index.count(); ++group) {
			const auto& catalog = index.catalogs()[group];
			for (std::uint32_t entry = 0; entry < catalog.count; ++entry)
				if (catalog.entries[entry].scratchBytes > result)
					result = catalog.entries[entry].scratchBytes;
		}
		return result;
	}

	const Fields* fields_;
	const Commands* commands_;
	const Services* services_;
};

} // namespace detail

// A device's Model: the group tree, Model{branches...} with group(name,
// items...), defined in GroupTree.hpp. Paths and handles (TreePath.hpp)
// name its endpoints.
template<class Fields, class Commands, class Services>
class Model;

} // namespace telemetry

#include "GroupTree.hpp"

#endif

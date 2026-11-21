/*
 * @file StructuredAbi.hpp
 * @brief Exact link-time tag for in-memory structured descriptor views.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Link-time compatibility for C++ views passed between separately built modules.
 *
 * The tag contains the revision, sizes, alignments, offsets and every project
 * switch of Config.hpp (local-storage budget, type names) used at the boundary.
 * Its exact template arguments name the required symbol, so mismatched modules
 * fail to link. It is independent of wire format versioning and adds no work
 * to a native typed endpoint call.
 */

#ifndef TELEMETRY_ABI_HPP
#define TELEMETRY_ABI_HPP
#pragma once

#include "../Config.hpp"
#include "../model/Model.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace telemetry {

// This describes the C++ in-memory ABI, not the version of descriptor.bin.
inline constexpr std::uint32_t structuredAbiRevision = 6;

static_assert(
    std::is_standard_layout_v<MemberDescriptor> && std::is_standard_layout_v<EnumEntryDescriptor> &&
        std::is_standard_layout_v<TypeDescriptor> && std::is_standard_layout_v<TypeRegistryView> &&
        std::is_standard_layout_v<ServiceEntry> && std::is_standard_layout_v<ServiceCatalog> &&
        std::is_standard_layout_v<ServiceIndex> && std::is_standard_layout_v<ServiceTypePair> &&
        std::is_standard_layout_v<ServiceTypeCatalog> && std::is_standard_layout_v<ModelView> &&
        std::is_standard_layout_v<EncodedCallResult> &&
        std::is_trivially_copyable_v<MemberDescriptor> &&
        std::is_trivially_copyable_v<EnumEntryDescriptor> &&
        std::is_trivially_copyable_v<TypeDescriptor> &&
        std::is_trivially_copyable_v<TypeRegistryView> &&
        std::is_trivially_copyable_v<ServiceEntry> &&
        std::is_trivially_copyable_v<ServiceCatalog> &&
        std::is_trivially_copyable_v<ServiceIndex> &&
        std::is_trivially_copyable_v<ServiceTypePair> &&
        std::is_trivially_copyable_v<ServiceTypeCatalog> &&
        std::is_trivially_copyable_v<ModelView> && std::is_trivially_copyable_v<EncodedCallResult>,
    "Structured ABI views must be standard-layout and trivially copyable");

static_assert(
    std::is_standard_layout_v<FieldEntry> && std::is_trivially_copyable_v<FieldEntry> &&
        std::is_standard_layout_v<CommandEntry> && std::is_trivially_copyable_v<CommandEntry> &&
        std::is_standard_layout_v<FieldCatalog> && std::is_trivially_copyable_v<FieldCatalog> &&
        std::is_standard_layout_v<CommandCatalog> && std::is_trivially_copyable_v<CommandCatalog> &&
        std::is_standard_layout_v<FieldIndex> && std::is_trivially_copyable_v<FieldIndex> &&
        std::is_standard_layout_v<CommandIndex> && std::is_trivially_copyable_v<CommandIndex> &&
        std::is_standard_layout_v<ValueTypeCatalog> &&
        std::is_trivially_copyable_v<ValueTypeCatalog> &&
        std::is_standard_layout_v<EncodedReadResult> &&
        std::is_trivially_copyable_v<EncodedReadResult> &&
        std::is_standard_layout_v<EncodedWriteResult> &&
        std::is_trivially_copyable_v<EncodedWriteResult> &&
        std::is_standard_layout_v<EncodedCommandResult> &&
        std::is_trivially_copyable_v<EncodedCommandResult>,
    "Field/Command views must be standard-layout and trivially copyable");

namespace detail {

// Exact layout pack used to name compiled boundary symbols.
template<std::uint64_t... Parts>
struct StructuredAbiTag {};

// Every descriptor field consumed across a translation-unit boundary is in
// this tuple. No hash is used for matching: the entire pack names the symbol.
// The Config.hpp switches (local bytes, type names) are part of it, so one
// object built with names on and one with names off cannot be linked.
using CurrentStructuredAbiTag = StructuredAbiTag<
    structuredAbiRevision, sizeof(void*), sizeof(std::string_view), alignof(std::string_view),
    sizeof(TypeKind), sizeof(ScalarCode), sizeof(TypeId), sizeof(MemberDescriptor),
    alignof(MemberDescriptor), offsetof(MemberDescriptor, typeId), offsetof(MemberDescriptor, name),
    sizeof(EnumEntryDescriptor), alignof(EnumEntryDescriptor),
    offsetof(EnumEntryDescriptor, codeBits), offsetof(EnumEntryDescriptor, name),
    sizeof(TypeDescriptor), alignof(TypeDescriptor), offsetof(TypeDescriptor, id),
    offsetof(TypeDescriptor, kind), offsetof(TypeDescriptor, wireBytes),
    offsetof(TypeDescriptor, recordBytes), offsetof(TypeDescriptor, scalarCode),
    offsetof(TypeDescriptor, relatedTypeId), offsetof(TypeDescriptor, elementCount),
    offsetof(TypeDescriptor, memberData), offsetof(TypeDescriptor, memberCount),
    offsetof(TypeDescriptor, enumData), offsetof(TypeDescriptor, enumCount),
    sizeof(TypeRegistryView), alignof(TypeRegistryView), offsetof(TypeRegistryView, data),
    offsetof(TypeRegistryView, count), offsetof(TypeRegistryView, recordsBytes),
    sizeof(ServiceEntry), alignof(ServiceEntry), offsetof(ServiceEntry, context),
    offsetof(ServiceEntry, invoke), offsetof(ServiceEntry, name),
    offsetof(ServiceEntry, requestWireBytes), offsetof(ServiceEntry, responseWireBytes),
    offsetof(ServiceEntry, scratchBytes), sizeof(ServiceCatalog), alignof(ServiceCatalog),
    offsetof(ServiceCatalog, name), offsetof(ServiceCatalog, entries),
    offsetof(ServiceCatalog, count), sizeof(ServiceIndex), alignof(ServiceIndex),
    offsetof(ServiceIndex, catalogs_), offsetof(ServiceIndex, count_), sizeof(ServiceTypePair),
    alignof(ServiceTypePair), offsetof(ServiceTypePair, requestTypeId),
    offsetof(ServiceTypePair, responseTypeId), sizeof(ServiceTypeCatalog),
    alignof(ServiceTypeCatalog), offsetof(ServiceTypeCatalog, entries),
    offsetof(ServiceTypeCatalog, count), sizeof(ModelView), alignof(ModelView),
    offsetof(ModelView, types), offsetof(ModelView, services), offsetof(ModelView, serviceTypes),
    offsetof(ModelView, serviceCatalogCount), sizeof(EncodedCallResult), alignof(EncodedCallResult),
    offsetof(EncodedCallResult, dispatch), offsetof(EncodedCallResult, endpointStatus),
    offsetof(EncodedCallResult, written), sizeof(std::span<const std::byte>),
    alignof(std::span<const std::byte>), sizeof(std::span<std::byte>),
    alignof(std::span<std::byte>), sizeof(Workspace), alignof(Workspace), sizeof(FieldEntry),
    alignof(FieldEntry), offsetof(FieldEntry, readContext), offsetof(FieldEntry, read),
    offsetof(FieldEntry, write), offsetof(FieldEntry, name), offsetof(FieldEntry, wireBytes),
    offsetof(FieldEntry, readScratchBytes), sizeof(CommandEntry), alignof(CommandEntry),
    offsetof(CommandEntry, context), offsetof(CommandEntry, invoke), offsetof(CommandEntry, name),
    offsetof(CommandEntry, requestWireBytes), offsetof(CommandEntry, scratchBytes),
    sizeof(FieldCatalog), alignof(FieldCatalog), offsetof(FieldCatalog, name),
    offsetof(FieldCatalog, entries), offsetof(FieldCatalog, count), sizeof(CommandCatalog),
    alignof(CommandCatalog), offsetof(CommandCatalog, name), offsetof(CommandCatalog, entries),
    offsetof(CommandCatalog, count), sizeof(FieldIndex), alignof(FieldIndex),
    offsetof(FieldIndex, catalogs_), offsetof(FieldIndex, count_), sizeof(CommandIndex),
    alignof(CommandIndex), offsetof(CommandIndex, catalogs_), offsetof(CommandIndex, count_),
    sizeof(ValueTypeCatalog), alignof(ValueTypeCatalog), offsetof(ValueTypeCatalog, entries),
    offsetof(ValueTypeCatalog, count), offsetof(ModelView, fields), offsetof(ModelView, commands),
    offsetof(ModelView, fieldTypes), offsetof(ModelView, fieldCatalogCount),
    offsetof(ModelView, commandTypes), offsetof(ModelView, commandCatalogCount),
    sizeof(EncodedReadResult), alignof(EncodedReadResult), offsetof(EncodedReadResult, dispatch),
    offsetof(EncodedReadResult, written), sizeof(EncodedWriteResult), alignof(EncodedWriteResult),
    offsetof(EncodedWriteResult, dispatch), offsetof(EncodedWriteResult, endpointStatus),
    sizeof(EncodedCommandResult), alignof(EncodedCommandResult),
    offsetof(EncodedCommandResult, dispatch), offsetof(EncodedCommandResult, endpointStatus),
    maxLocalObjectBytes, offsetof(FieldEntry, writeContext),
    offsetof(FieldEntry, writeScratchBytes), std::uint64_t{TELEMETRY_TYPE_NAMES}>;

// Only the exact current specialization is defined in StructuredAbi.cpp.
template<class Tag>
void requireStructuredAbi(Tag) noexcept;

template<>
void requireStructuredAbi<CurrentStructuredAbiTag>(CurrentStructuredAbiTag) noexcept;

} // namespace detail

// Call once at a module boundary that exchanges structured descriptor views.
// It has no place in native read/write/service hot paths.
template<class Tag = detail::CurrentStructuredAbiTag>
inline void requireStructuredAbi() noexcept
{
	detail::requireStructuredAbi(Tag{});
}

} // namespace telemetry

#endif

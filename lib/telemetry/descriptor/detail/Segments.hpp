/*
 * @file Segments.hpp
 * @brief Shared canonical emission for indexed streaming and constexpr packing.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Use one canonical emitter for fingerprinting, streaming and packing so
 * those paths cannot choose different byte layouts. Sinks own their progress;
 * segments carry offsets and IDs while ModelView keeps borrowed metadata.
 */
#ifndef TELEMETRY_DESCRIPTOR_DETAIL_SEGMENTS_HPP
#define TELEMETRY_DESCRIPTOR_DETAIL_SEGMENTS_HPP
#pragma once

#include "../Format.hpp"
#include <telemetry/model/Model.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace telemetry::descriptor::detail {
namespace ts = ::telemetry;

enum class SegmentKind : std::uint8_t {
	Header,
	Type,
	Member,
	EnumEntry,
	FieldCatalog,
	CommandCatalog,
	ServiceCatalog,
	Field,
	Command,
	Service,
	GroupCatalog
};

// Twelve bytes per segment on every supported ABI. Metadata addresses remain
// in ModelView; offsets permit binary-search resume inside a long string.
// The low nibble is kind, bit 4 is Field writable, upper bits are a member/
// enum index or a catalog's first endpoint ordinal. No pointer is serialized.
// Public methods:
// - kind(): Decode segment category.
// - index(): Decode segment ordinal.
struct Segment {
	std::uint32_t offset;
	std::uint32_t key;
	std::uint32_t detail;

	[[nodiscard]] constexpr SegmentKind kind() const noexcept
	{
		return static_cast<SegmentKind>(detail & 15);
	}

	[[nodiscard]] constexpr std::uint32_t index() const noexcept
	{
		return detail >> 5;
	}
};

static_assert(sizeof(Segment) == 12);

// typeNames borrows the registry's static names indexed by TypeId. It is
// empty when the descriptor emits no names. groupDepths borrows the tree
// Model's static outline and is empty for a flat model.
struct Header {
	std::uint32_t totalBytes = 0;
	std::uint64_t fingerprint = 0;
	std::uint32_t typeCount = 0;
	std::array<std::uint32_t, 3> catalogCount{};
	std::array<std::uint32_t, 3> endpointCount{};
	std::uint32_t catalogsOffset = 0;
	std::uint32_t endpointsOffset = 0;
	std::uint16_t minor = binaryMinor;
	std::span<const std::string_view> typeNames{};
	std::span<const std::uint16_t> groupDepths{};
};

// The emitted name of one type record: empty for an unnamed type and for
// every type when the descriptor strips names.
[[nodiscard]] constexpr std::string_view typeName(const Header& header, std::uint32_t id) noexcept
{
	return id < header.typeNames.size() ? header.typeNames[id] : std::string_view{};
}

// Hash the same bytes that SliceSink emits; the caller controls when the
// fingerprint field changes from zero to its completed value.
// Public methods:
// - byte(): Hash one byte.
// - integer(): Hash ordered integer.
// - text(): Hash UTF8 bytes.
struct HashSink {
	std::uint64_t value = fingerprintBasis;

	constexpr void byte(std::uint8_t next) noexcept
	{
		value = (value ^ next) * fingerprintPrime;
	}

	constexpr void integer(std::uint64_t next, unsigned count) noexcept
	{
		for (unsigned i = 0; i < count; ++i) {
			byte(static_cast<std::uint8_t>(next >> (i * 8)));
		}
	}

	constexpr void text(std::string_view next) noexcept
	{
		for (unsigned char c : next) {
			byte(c);
		}
	}
};

// Skipping a borrowed string is constant work. Only emitted bytes are read;
// small fixed numeric prefixes cost bounded work per visited segment.
// Public methods:
// - byte(): Emit fitting byte.
// - integer(): Emit ordered integer.
// - text(): Emit borrowed string.
struct SliceSink {
	std::span<std::byte> output;
	std::uint32_t skip;
	std::uint32_t written = 0;

	constexpr void byte(std::uint8_t next) noexcept
	{
		if (skip != 0) {
			--skip;
		} else if (written < output.size()) {
			output[written++] = static_cast<std::byte>(next);
		}
	}

	constexpr void integer(std::uint64_t next, unsigned count) noexcept
	{
		for (unsigned i = 0; i < count; ++i) {
			byte(static_cast<std::uint8_t>(next >> (i * 8)));
		}
	}

	constexpr void text(std::string_view next) noexcept
	{
		if (skip >= next.size()) {
			skip -= static_cast<std::uint32_t>(next.size());
			return;
		}
		next.remove_prefix(skip);
		skip = 0;
		const auto available = output.size() - written;
		const auto count = next.size() < available ? next.size() : available;
		for (std::size_t i = 0; i < count; ++i) {
			output[written++] = static_cast<std::byte>(static_cast<unsigned char>(next[i]));
		}
	}
};

template<class Sink>
constexpr void record(Sink& sink, RecordKind kind, std::uint32_t bytes) noexcept
{
	sink.integer(static_cast<unsigned>(kind), 1);
	sink.integer(recordVersion, 1);
	sink.integer(0, 2);
	sink.integer(bytes - recordHeaderBytes, 4);
}

template<class Sink>
constexpr void string(Sink& sink, std::string_view text) noexcept
{
	sink.integer(text.size(), 4);
	sink.text(text);
}

// During construction offset temporarily holds segment length. The final
// offset pass happens after name checks, so sorting cannot invalidate lengths.
[[nodiscard]] constexpr std::string_view segmentName(const Segment& part, std::uint32_t bytes,
                                                     const ts::ModelView& model) noexcept
{
	const auto group = part.key >> 16;
	const auto entry = part.key & 0xffff;
	switch (part.kind()) {
		case SegmentKind::FieldCatalog:
			return {model.fields.catalogs()[part.key].name, bytes - 28};
		case SegmentKind::CommandCatalog:
			return {model.commands.catalogs()[part.key].name, bytes - 28};
		case SegmentKind::ServiceCatalog:
			return {model.services.catalogs()[part.key].name, bytes - 28};
		case SegmentKind::GroupCatalog:
			return {model.fields.catalogs()[part.key].name, bytes - 28};
		case SegmentKind::Field:
			return {model.fields.catalogs()[group].entries[entry].name, bytes - 24};
		case SegmentKind::Command:
			return {model.commands.catalogs()[group].entries[entry].name, bytes - 20};
		case SegmentKind::Service:
			return {model.services.catalogs()[group].entries[entry].name, bytes - 24};
		default:
			return {};
	}
}

// part/bytes must come from the validated positional index. The emitter
// performs no lookup validation and never dereferences an endpoint owner.
template<class Sink>
constexpr void emit(const Segment& part, std::uint32_t bytes, const ts::ModelView& model,
                    const Header& header, Sink& sink) noexcept
{
	const auto group = part.key >> 16;
	const auto entry = part.key & 0xffff;
	switch (part.kind()) {
		case SegmentKind::Header:
			sink.text("TDS3");
			sink.integer(binaryMajor, 2);
			sink.integer(header.minor, 2);
			sink.integer(descriptorHeaderBytes, 2);
			sink.integer(
			    header.groupDepths.empty() ? 0u : static_cast<unsigned>(HeaderFlag::GroupTree), 2);
			sink.integer(header.totalBytes, 4);
			sink.integer(header.fingerprint, 8);
			sink.integer(header.typeCount, 4);
			for (unsigned i = 0; i < 3; ++i) {
				sink.integer(header.catalogCount[i], 4);
				sink.integer(header.endpointCount[i], 4);
			}
			sink.integer(descriptorHeaderBytes, 4);
			sink.integer(header.catalogsOffset, 4);
			sink.integer(header.endpointsOffset, 4);
			return;
		case SegmentKind::Type: {
			const auto& type = model.types.data[part.key];
			// v3.1: a named type sets TypeFlag::Named and places its name
			// right after the 12-byte prefix; recordBytes excludes the name.
			const auto name = typeName(header, part.key);
			const auto named = name.empty() ? 0u : 4u + static_cast<std::uint32_t>(name.size());
			record(sink, RecordKind::Type, type.recordBytes + named);
			sink.integer(type.id, 4);
			sink.integer(static_cast<unsigned>(type.kind), 1);
			sink.integer(named == 0 ? 0u : static_cast<unsigned>(TypeFlag::Named), 1);
			sink.integer(0, 2);
			sink.integer(type.wireBytes, 4);
			if (named != 0) {
				string(sink, name);
			}
			switch (type.kind) {
				case ts::TypeKind::Void:
					break;
				case ts::TypeKind::Scalar:
					sink.integer(static_cast<unsigned>(type.scalarCode), 1);
					sink.integer(0, 3);
					break;
				case ts::TypeKind::Enum:
					sink.integer(type.relatedTypeId, 4);
					sink.integer(type.enumCount, 4);
					break;
				case ts::TypeKind::Struct:
					sink.integer(type.memberCount, 4);
					break;
				case ts::TypeKind::Array:
					sink.integer(type.relatedTypeId, 4);
					sink.integer(type.elementCount, 4);
					break;
			}
			return;
		}
		case SegmentKind::Member: {
			const auto& member = model.types.data[part.key].memberData[part.index()];
			sink.integer(member.typeId, 4);
			string(sink, member.name);
			return;
		}
		case SegmentKind::EnumEntry: {
			const auto& type = model.types.data[part.key];
			const auto& value = type.enumData[part.index()];
			sink.integer(value.codeBits, type.wireBytes);
			string(sink, value.name);
			return;
		}
		case SegmentKind::FieldCatalog:
		case SegmentKind::CommandCatalog:
		case SegmentKind::ServiceCatalog: {
			const auto category = static_cast<unsigned>(part.kind()) -
			                      static_cast<unsigned>(SegmentKind::FieldCatalog);
			const auto count = category == 0   ? model.fields.catalogs()[part.key].count
			                   : category == 1 ? model.commands.catalogs()[part.key].count
			                                   : model.services.catalogs()[part.key].count;
			record(sink, RecordKind::Catalog, bytes);
			sink.integer(category + 1, 1);
			sink.integer(0, 3);
			sink.integer(part.key, 4);
			sink.integer(part.index(), 4);
			sink.integer(count, 4);
			string(sink, segmentName(part, bytes, model));
			return;
		}
		case SegmentKind::GroupCatalog:
			// One group of the shared tree: every family's row at this position.
			record(sink, RecordKind::Catalog, bytes);
			sink.integer(groupCategory, 1);
			sink.integer(0, 1);
			sink.integer(header.groupDepths[part.key], 2);
			sink.integer(model.fields.catalogs()[part.key].count, 4);
			sink.integer(model.commands.catalogs()[part.key].count, 4);
			sink.integer(model.services.catalogs()[part.key].count, 4);
			string(sink, segmentName(part, bytes, model));
			return;
		case SegmentKind::Field:
			record(sink, RecordKind::Field, bytes);
			sink.integer(part.key, 4);
			sink.integer(model.fieldTypes[group].entries[entry], 4);
			sink.integer((part.detail & 16) != 0 ? 3 : 1, 1);
			sink.integer(0, 3);
			string(sink, segmentName(part, bytes, model));
			return;
		case SegmentKind::Command:
			record(sink, RecordKind::Command, bytes);
			sink.integer(part.key, 4);
			sink.integer(model.commandTypes[group].entries[entry], 4);
			string(sink, segmentName(part, bytes, model));
			return;
		case SegmentKind::Service: {
			const auto types = model.serviceTypes[group].entries[entry];
			record(sink, RecordKind::Service, bytes);
			sink.integer(part.key, 4);
			sink.integer(types.requestTypeId, 4);
			sink.integer(types.responseTypeId, 4);
			string(sink, segmentName(part, bytes, model));
			return;
		}
	}
}

} // namespace telemetry::descriptor::detail
#endif

/*
 * @file ValuesReader.hpp
 * @brief Reads a values.bin snapshot against the descriptor it was written with.
 * @author Ruslan Kovtun (shpegun60)
 * SPDX-License-Identifier: MIT
 *
 * The host's side of ValuesFile: the same TVL3 layout (doc/WireV3.md, "Values
 * header and tokens"), read where a client has the file - a download, a file
 * on disk - and its descriptor parsed into a RuntimeSchema. The header alone
 * names that descriptor (its fingerprint), so a reader can find it first.
 *
 * The rules are telemetry.js decodeValues's: magic, version 3.0, the Field
 * count and the file's size as the header says, the fingerprint the schema's,
 * one token per Field in descriptor order - a status byte, Ok (0) or
 * Unavailable (1), then the Field's wire bytes, all zero when Unavailable.
 * The payloads are handed out as spans for DynamicValueView; nothing is
 * copied or allocated. No Qt.
 */
#ifndef RESOURCE_TELEMETRY_V3_VALUES_READER_HPP
#define RESOURCE_TELEMETRY_V3_VALUES_READER_HPP
#pragma once

#include <integration/telemetry/v3/ValuesFormat.hpp>
#include <telemetry/runtime/RuntimeSchema.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace resource::telemetry::v3 {
struct ValuesHeader {
	std::uint16_t major = 0, minor = 0;
	std::uint32_t fieldCount = 0, totalBytes = 0;
	std::uint64_t fingerprint = 0;
};

// Why bytes are not a values snapshot of a schema.
enum class ValuesProblem : std::uint8_t {
	None,
	Short,       // shorter than its header, or than its tokens
	Magic,       // not "TVL3"
	Version,     // not 3.0
	Size,        // the header's totalBytes is not the file's size
	Fingerprint, // written with another descriptor
	Count,       // another number of Fields than the descriptor has
	Layout,      // the tokens do not add up to the size
	Status,      // a status byte other than Ok or Unavailable
	Padding      // an Unavailable value whose bytes are not all zero
};

// One Field's token: its endpoint in the schema, its status, its wire bytes
// (zeros when Unavailable), and where its status byte is in the file.
struct ValuesEntry {
	const ::telemetry::runtime::RuntimeEndpoint* endpoint = nullptr;
	ValueStatus status = ValueStatus::Ok;
	std::span<const std::byte> payload{};
	std::uint32_t offset = 0;
};

namespace values_detail {
inline std::uint64_t little(std::span<const std::byte> bytes, std::size_t at,
                            std::size_t width) noexcept
{
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < width; ++i)
		value |= static_cast<std::uint64_t>(bytes[at + i]) << (8 * i);
	return value;
}
} // namespace values_detail

// The header alone: which descriptor the snapshot needs (its fingerprint).
inline ValuesProblem readValuesHeader(std::span<const std::byte> bytes, ValuesHeader& out) noexcept
{
	out = {};
	if (bytes.size() < valuesHeaderBytes)
		return ValuesProblem::Short;
	for (std::size_t i = 0; i < valuesMagic.size(); ++i)
		if (bytes[i] != valuesMagic[i])
			return ValuesProblem::Magic;
	out.major = static_cast<std::uint16_t>(values_detail::little(bytes, 4, 2));
	out.minor = static_cast<std::uint16_t>(values_detail::little(bytes, 6, 2));
	out.fieldCount = static_cast<std::uint32_t>(values_detail::little(bytes, 8, 4));
	out.totalBytes = static_cast<std::uint32_t>(values_detail::little(bytes, 12, 4));
	out.fingerprint = values_detail::little(bytes, 16, 8);
	if (out.major != valuesMajor || out.minor != valuesMinor)
		return ValuesProblem::Version;
	if (out.totalBytes != bytes.size())
		return ValuesProblem::Size;
	return ValuesProblem::None;
}

// Checks the whole snapshot against schema, then calls visit(const
// ValuesEntry&) for every Field in descriptor order. Nothing is visited unless
// the whole file is right.
template<class Visit>
ValuesProblem readValues(std::span<const std::byte> bytes,
                         const ::telemetry::runtime::RuntimeSchema& schema, Visit&& visit) noexcept
{
	ValuesHeader header;
	if (const auto problem = readValuesHeader(bytes, header); problem != ValuesProblem::None)
		return problem;
	if (header.fingerprint != schema.fingerprint())
		return ValuesProblem::Fingerprint;
	const auto fields = schema.endpoints(::telemetry::descriptor::Category::Field);
	if (header.fieldCount != fields.size())
		return ValuesProblem::Count;
	// First the layout and every status, so a bad file visits nothing.
	std::size_t at = valuesHeaderBytes;
	for (const auto& field : fields) {
		const auto* type = schema.type(field.typeId);
		if (!type || at + 1 + type->wireBytes > bytes.size())
			return ValuesProblem::Short;
		const auto status = static_cast<std::uint8_t>(bytes[at]);
		if (status != static_cast<std::uint8_t>(ValueStatus::Ok) &&
		    status != static_cast<std::uint8_t>(ValueStatus::Unavailable))
			return ValuesProblem::Status;
		if (status == static_cast<std::uint8_t>(ValueStatus::Unavailable))
			for (std::size_t i = 0; i < type->wireBytes; ++i)
				if (bytes[at + 1 + i] != std::byte{0})
					return ValuesProblem::Padding;
		at += 1 + type->wireBytes;
	}
	if (at != bytes.size())
		return ValuesProblem::Layout;
	at = valuesHeaderBytes;
	for (const auto& field : fields) {
		const auto width = schema.type(field.typeId)->wireBytes;
		visit(ValuesEntry{&field, static_cast<ValueStatus>(bytes[at]), bytes.subspan(at + 1, width),
		                  static_cast<std::uint32_t>(at)});
		at += 1 + width;
	}
	return ValuesProblem::None;
}

// The problem in plain English, for a client's message.
constexpr std::string_view describe(ValuesProblem problem) noexcept
{
	switch (problem) {
		case ValuesProblem::None:
			return "a values snapshot";
		case ValuesProblem::Short:
			return "the file ends early";
		case ValuesProblem::Magic:
			return "not a values file";
		case ValuesProblem::Version:
			return "a values version this program does not read";
		case ValuesProblem::Size:
			return "its size does not match its header";
		case ValuesProblem::Fingerprint:
			return "it was written with another descriptor";
		case ValuesProblem::Count:
			return "it holds another number of Fields than its descriptor";
		case ValuesProblem::Layout:
			return "its values do not add up to its size";
		case ValuesProblem::Status:
			return "a value has a status this program does not know";
		case ValuesProblem::Padding:
			return "an unavailable value is not blank";
	}
	return "unknown";
}
} // namespace resource::telemetry::v3
#endif

/*
 * @file Format.hpp
 * @brief Canonical structural descriptor v3.0/v3.1 wire constants.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Keep the producer and client on one frozen descriptor and values layout.
 * These numbers describe canonical bytes, independently of C++ object
 * layout; version changes must be coordinated with descriptor readers.
 * v3.1 adds three optional parts to v3.0: a type name on Enum and Struct
 * records, a skippable attribute area after any record's payload, and the
 * GroupTree mode, where one shared tree of group records replaces the three
 * family catalogs. Without them, a descriptor is byte-identical v3.0 (minor 0).
 */
#ifndef TELEMETRY_DESCRIPTOR_FORMAT_HPP
#define TELEMETRY_DESCRIPTOR_FORMAT_HPP
#pragma once

#include <cstdint>

namespace telemetry::descriptor {

inline constexpr std::uint16_t binaryMajor = 3;
// The base minor: a descriptor without type names or attributes, and every
// Values file. Values (TVL3) stays 3.0 and pairs with its descriptor by
// fingerprint, not by version.
inline constexpr std::uint16_t binaryMinor = 0;
// The descriptor minor when some type is named, some record has attributes or
// the catalogs are one group tree.
inline constexpr std::uint16_t typeNamesMinor = 1;
inline constexpr std::uint16_t descriptorHeaderBytes = 64;
inline constexpr std::uint8_t recordVersion = 1;
inline constexpr std::uint32_t recordHeaderBytes = 8;
inline constexpr std::uint64_t fingerprintBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t fingerprintPrime = 0x100000001b3ULL;

enum class RecordKind : std::uint8_t {
	Type = 1,
	Catalog,
	Field,
	Command,
	Service
};
enum class Category : std::uint8_t {
	Field = 1,
	Command,
	Service
};
enum class Capability : std::uint8_t {
	Readable = 1,
	Writable = 2
};

// v3.1: the first byte after typeKind in the Type prefix (zero in v3.0).
// Named means a string follows the 12-byte prefix; Enum and Struct only.
enum class TypeFlag : std::uint8_t {
	Named = 1
};

// v3.1: the u16 flags of every record header (zero in v3.0). Attributes
// means at least one {u8 tag, u32 length, value} follows the payload and runs
// exactly to the end of payloadBytes. Tags ascend strictly, so each appears at
// most once per record and its value describes the whole record. Never on
// Void/Scalar. In a Struct record a member tag's value is a presence bitmap
// of ceil(memberCount / 8) bytes (bit i = member i), then one item per set
// bit in member order.
enum class RecordFlag : std::uint16_t {
	Attributes = 1
};

inline constexpr std::uint32_t attributeHeaderBytes = 5;

// v3.1: the u16 flags of the descriptor header (zero in v3.0). GroupTree means
// the Catalogs section holds one group record per group of a shared tree, in
// pre-order, and the three catalog counts of the header all equal that count.
enum class HeaderFlag : std::uint16_t {
	GroupTree = 1
};

// The category byte of a group record (v3.0 catalog records keep 1..3):
//   u8 category 0, u8 reserved 0, u16 depth, u32 fieldCount, u32 commandCount,
//   u32 serviceCount, string name
// The first depth is 0 and each is at most the previous one plus 1; a group's
// first entry in each family is the sum of the earlier groups' counts.
inline constexpr std::uint8_t groupCategory = 0;

// Attribute tag ranges. A host skips an advisory tag it does not know
// (validating order and bounds, skipping the value by its length), refuses a
// reserved tag, and refuses the descriptor for an unknown critical tag. v3.1
// defines no tag yet; Range, Unit and Default are reserved for their own
// reviewed contracts.
inline constexpr std::uint8_t sdkAdvisoryTagFirst = 0x01;
inline constexpr std::uint8_t sdkAdvisoryTagLast = 0x3f;
inline constexpr std::uint8_t applicationTagFirst = 0x40;
inline constexpr std::uint8_t applicationTagLast = 0x7f;
inline constexpr std::uint8_t criticalTagFirst = 0x81;
inline constexpr std::uint8_t criticalTagLast = 0xbf;
inline constexpr std::uint8_t rangeTag = 0x01;
inline constexpr std::uint8_t unitTag = 0x02;
inline constexpr std::uint8_t defaultTag = 0x03;

enum class AttributeTagClass : std::uint8_t {
	Reserved,
	Advisory,
	Application,
	Critical
};

// 0x00, 0x80 and 0xC0..0xFF are reserved.
[[nodiscard]] constexpr AttributeTagClass attributeTagClass(std::uint8_t tag) noexcept
{
	if (tag >= sdkAdvisoryTagFirst && tag <= sdkAdvisoryTagLast)
		return AttributeTagClass::Advisory;
	if (tag >= applicationTagFirst && tag <= applicationTagLast)
		return AttributeTagClass::Application;
	if (tag >= criticalTagFirst && tag <= criticalTagLast)
		return AttributeTagClass::Critical;
	return AttributeTagClass::Reserved;
}

} // namespace telemetry::descriptor
#endif

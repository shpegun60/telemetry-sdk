/*
 * @file Format.hpp
 * @brief Canonical structural descriptor v3.0/v3.1 wire constants.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Keep the producer and client on one frozen descriptor and values layout.
 * These numbers describe canonical bytes, independently of C++ object
 * layout; version changes must be coordinated with descriptor readers.
 * v3.1 adds two optional parts to v3.0: a type name on Enum and Struct
 * records, and a skippable attribute area after any record's payload.
 * Without both, a descriptor is byte-identical v3.0 (minor 0).
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
// The descriptor minor when some type is named or some record has attributes.
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
// means at least one {u16 tag, u16 target, u32 length, value} follows the
// payload and runs exactly to the end of payloadBytes. Never on Void/Scalar.
enum class RecordFlag : std::uint16_t {
	Attributes = 1
};

inline constexpr std::uint32_t attributeHeaderBytes = 8;

// Attribute tag ranges. A host skips an advisory tag it does not know
// (validating order, target and bounds), refuses a reserved tag, and refuses
// the descriptor for an unknown critical tag. v3.1 defines no tag yet; 0x0001
// (Range) and 0x0002 (Unit) are reserved for their own reviewed contracts.
inline constexpr std::uint16_t sdkAdvisoryTagFirst = 0x0001;
inline constexpr std::uint16_t sdkAdvisoryTagLast = 0x3fff;
inline constexpr std::uint16_t applicationTagFirst = 0x4000;
inline constexpr std::uint16_t applicationTagLast = 0x7fff;
inline constexpr std::uint16_t criticalTagFirst = 0x8001;
inline constexpr std::uint16_t criticalTagLast = 0xbfff;
inline constexpr std::uint16_t rangeTag = 0x0001;
inline constexpr std::uint16_t unitTag = 0x0002;

enum class AttributeTagClass : std::uint8_t {
	Reserved,
	Advisory,
	Application,
	Critical
};

// 0x0000, 0x8000 and 0xC000..0xFFFF are reserved.
[[nodiscard]] constexpr AttributeTagClass attributeTagClass(std::uint16_t tag) noexcept
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

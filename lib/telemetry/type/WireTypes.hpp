/*
 * @file WireTypes.hpp
 * @brief Structural wire kinds and fixed format limits without native reflection.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_TYPE_WIRE_TYPES_HPP
#define TELEMETRY_TYPE_WIRE_TYPES_HPP
#pragma once

#include <cstdint>

namespace telemetry {

using TypeId = std::uint32_t;

// A missing request/response has no object and consumes no wire bytes.
using Void = void;

enum class TypeKind : std::uint8_t {
	Void = 0,
	Scalar = 1,
	Enum = 2,
	Struct = 3,
	Array = 4
};

enum class ScalarCode : std::uint8_t {
	Bool = 1,
	U8,
	S8,
	U16,
	S16,
	U32,
	S32,
	U64,
	S64,
	F32,
	F64
};

// Fixed structural ceilings bound compile-time metadata and expanded value
// traversal. They are format/library limits, not application parameter ranges
// or a promise about the total runtime stack of user callbacks.
struct Limits {
	static constexpr std::uint32_t maxTypeDepth = 32;
	static constexpr std::uint32_t maxTypeCount = 4096;
	static constexpr std::uint32_t maxStructMembers = 256;
	static constexpr std::uint32_t maxArrayElements = 65536;
	static constexpr std::uint32_t maxDescriptorBytes = 4194304;
	static constexpr std::uint32_t maxStringBytes = 4096;
	static constexpr std::uint32_t maxEnumEntriesTotal = 65536;
	static constexpr std::uint32_t maxCatalogCountTotal = 65536;
	static constexpr std::uint32_t maxEndpointCountTotal = 65536;
	static constexpr std::uint32_t maxValueWireBytes = 1048576;
	static constexpr std::uint32_t maxExpandedValueNodes = 262144;
	// Group-tree ceilings (descriptor v3.1). A producer is strict so that new
	// devices stay tidy; hosts read with ParseLimits' lenient 255/4096/4096.
	// maxGroupDepth counts levels: depths 0..maxGroupDepth-1. maxNameBytes
	// bounds one group, endpoint or member name in a tree and one part of a
	// type name; maxPathBytes bounds group/.../endpoint and a whole type name.
	static constexpr std::uint32_t maxGroupDepth = 8;
	static constexpr std::uint32_t maxNameBytes = 64;
	static constexpr std::uint32_t maxPathBytes = 256;
};

} // namespace telemetry
#endif // TELEMETRY_TYPE_WIRE_TYPES_HPP

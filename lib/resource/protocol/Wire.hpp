/**
 * @file Wire.hpp
 * @brief Stable byte counts and operation codes for resource packets v1.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see ../LICENSE.
 */
#ifndef TELEMETRY_LIB_RESOURCE_PROTOCOL_WIRE_HPP
#define TELEMETRY_LIB_RESOURCE_PROTOCOL_WIRE_HPP
#pragma once

#include <cstddef>
#include <cstdint>

namespace resource::protocol {
enum class Op : std::uint8_t {
	List = 1,
	Stat = 2,
	Read = 3,
	Write = 4,
	// Persist what the file holds now (what WRITE applied), in steps.
	Save = 5
};

namespace wire {
inline constexpr std::size_t listRequestSize = 9;
inline constexpr std::size_t statRequestSize = 5;
inline constexpr std::size_t readRequestSize = 13;
inline constexpr std::size_t writeRequestHeaderSize = 16;
inline constexpr std::size_t chunkReplyHeaderSize = 12;
inline constexpr std::size_t statReplySize = 6;
inline constexpr std::size_t writeReplySize = 14;
inline constexpr std::size_t saveRequestSize = 13;
inline constexpr std::size_t saveReplySize = 10;
inline constexpr std::size_t maxPayloadSize = 65535;
inline constexpr std::size_t maxListPathSize = maxPayloadSize - sizeof(std::uint16_t);
} // namespace wire
} // namespace resource::protocol

#endif // TELEMETRY_LIB_RESOURCE_PROTOCOL_WIRE_HPP

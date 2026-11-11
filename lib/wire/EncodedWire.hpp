/*
 * @file EncodedWire.hpp
 * @brief Complete-frame Encoded v1 request/reply contract, without transport state.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Each invocation has one request and one reply. There are no retries, polling,
 * duplicate suppression or connection admission in this byte contract. A trusted
 * owner binds the nonzero session to its admitted peer and supplies complete
 * frames. Parsed payloads borrow their unchanged input frame.
 */
#ifndef TELEMETRY_PROTOCOL_ENCODED_WIRE_HPP
#define TELEMETRY_PROTOCOL_ENCODED_WIRE_HPP
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace telemetry::protocol::encoded {

inline constexpr std::size_t headerBytes = 32;
inline constexpr std::uint32_t magic = 0x31514554u; // LE bytes: TEQ1.
inline constexpr std::uint8_t version = 1;

enum class Operation : std::uint8_t {
	FieldWrite = 1,
	Command = 2,
	Service = 3,
	FieldRead = 4,
	ReadMany = 5,
	Batch = 6
	// 7 is reserved and never valid on this wire: the client queue uses it for
	// raw Resource requests (client::Operation::Resource), sent without a header.
};

enum class Status : std::uint8_t {
	Ok,
	Truncated,
	InvalidMagic,
	InvalidVersion,
	InvalidFlags,
	InvalidOperation,
	InvalidHeader,
	InvalidLength,
	BufferTooSmall
};

struct Header {
	bool reply = false;
	Operation operation = Operation::FieldRead;
	std::uint8_t dispatch = 0;
	std::uint32_t session = 0;
	std::uint32_t requestId = 0;
	std::uint32_t endpoint = 0;
	std::uint32_t payloadBytes = 0;
	std::uint32_t expectedResponseBytes = 0;
	std::uint8_t endpointStatus = 0;
};

struct ParseResult {
	Status status = Status::Truncated;
	Header header{};
	std::span<const std::byte> payload{};

	[[nodiscard]] constexpr explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

struct BuildResult {
	Status status = Status::BufferTooSmall;
	std::size_t written = 0;

	[[nodiscard]] constexpr explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

namespace detail {
[[nodiscard]] constexpr std::uint32_t load32(std::span<const std::byte> bytes,
                                             std::size_t offset) noexcept
{
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
	return value;
}

constexpr void store32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept
{
	for (unsigned i = 0; i < 4; ++i)
		bytes[offset + i] = std::byte((value >> (8 * i)) & 0xffu);
}

[[nodiscard]] constexpr bool validDispatch(std::uint8_t value) noexcept
{
	return value == 0 || (value >= 4 && value <= 9);
}
} // namespace detail

// Status values match the existing Model encoded API, without importing a
// native codec or any client/server runtime into this shared byte contract.
[[nodiscard]] constexpr Status validate(const Header& header) noexcept
{
	const auto operation = static_cast<std::uint8_t>(header.operation);
	if (operation < 1 || operation > 6)
		return Status::InvalidOperation;
	if (header.session == 0 || header.requestId == 0 || !detail::validDispatch(header.dispatch))
		return Status::InvalidHeader;
	if ((header.operation == Operation::FieldWrite || header.operation == Operation::Command) &&
	    header.expectedResponseBytes != 0)
		return Status::InvalidHeader;
	// Only the optional batch operation has a response budget, rather than an
	// exact DTO size. Admission and complete body validation belong to its port.
	if (header.operation == Operation::ReadMany &&
	    (header.endpoint != 0 || header.endpointStatus != 0 || header.expectedResponseBytes < 10 ||
	     header.expectedResponseBytes > 514))
		return Status::InvalidHeader;
	// Mixed Batch is an independent optional capability. Its bound includes all
	// item headers and maximum successful DTOs; item results can be shorter.
	if (header.operation == Operation::Batch &&
	    (header.endpoint != 0 || header.endpointStatus != 0 || header.expectedResponseBytes < 14 ||
	     header.expectedResponseBytes > 65535 || header.payloadBytes > 65535))
		return Status::InvalidHeader;
	if (!header.reply) {
		if (header.dispatch != 0 || header.endpointStatus != 0 ||
		    (header.operation == Operation::FieldRead && header.payloadBytes != 0))
			return Status::InvalidHeader;
		return Status::Ok;
	}
	if (header.dispatch != 0)
		return header.endpointStatus == 0 && header.payloadBytes == 0 ? Status::Ok
		                                                              : Status::InvalidHeader;
	switch (header.operation) {
		case Operation::FieldWrite:
			return header.endpointStatus <= 5 && header.payloadBytes == 0 ? Status::Ok
			                                                              : Status::InvalidHeader;
		case Operation::Command:
			return header.endpointStatus <= 7 && header.payloadBytes == 0 ? Status::Ok
			                                                              : Status::InvalidHeader;
		case Operation::Service:
			if (header.endpointStatus > 4)
				return Status::InvalidHeader;
			return header.payloadBytes ==
			               (header.endpointStatus == 0 ? header.expectedResponseBytes : 0)
			           ? Status::Ok
			           : Status::InvalidHeader;
		case Operation::FieldRead:
			return header.endpointStatus == 0 && header.payloadBytes == header.expectedResponseBytes
			           ? Status::Ok
			           : Status::InvalidHeader;
		case Operation::ReadMany:
			return header.payloadBytes >= 2 && header.payloadBytes <= header.expectedResponseBytes
			           ? Status::Ok
			           : Status::InvalidHeader;
		case Operation::Batch:
			return header.payloadBytes >= 14 && header.payloadBytes <= header.expectedResponseBytes
			           ? Status::Ok
			           : Status::InvalidHeader;
	}
	return Status::InvalidOperation;
}

[[nodiscard]] constexpr ParseResult parse(std::span<const std::byte> frame) noexcept
{
	if (frame.size() < headerBytes)
		return {Status::Truncated};
	if (detail::load32(frame, 0) != magic)
		return {Status::InvalidMagic};
	if (frame[4] != std::byte{version})
		return {Status::InvalidVersion};
	const auto flags = std::to_integer<unsigned>(frame[5]);
	if (flags > 1)
		return {Status::InvalidFlags};
	if (frame[29] != std::byte{0} || frame[30] != std::byte{0} || frame[31] != std::byte{0})
		return {Status::InvalidHeader};
	Header header{flags == 1,
	              static_cast<Operation>(std::to_integer<unsigned>(frame[6])),
	              std::to_integer<std::uint8_t>(frame[7]),
	              detail::load32(frame, 8),
	              detail::load32(frame, 12),
	              detail::load32(frame, 16),
	              detail::load32(frame, 20),
	              detail::load32(frame, 24),
	              std::to_integer<std::uint8_t>(frame[28])};
	const auto status = validate(header);
	if (status != Status::Ok)
		return {status};
	if (frame.size() - headerBytes != header.payloadBytes)
		return {Status::InvalidLength};
	return {Status::Ok, header, frame.subspan(headerBytes)};
}

// Write only the header. The caller supplies exactly payloadBytes after it.
// On failure output is unchanged; unaligned output is supported.
[[nodiscard]] constexpr Status writeHeader(std::span<std::byte> output,
                                           const Header& header) noexcept
{
	const auto status = validate(header);
	if (status != Status::Ok)
		return status;
	if (output.size() < headerBytes)
		return Status::BufferTooSmall;
	// The destination may cover the native Header object. Preserve all source
	// fields before publishing bytes, including its bool representation.
	const Header value = header;
	detail::store32(output, 0, magic);
	output[4] = std::byte{version};
	output[5] = std::byte(value.reply ? 1 : 0);
	output[6] = std::byte(static_cast<std::uint8_t>(value.operation));
	output[7] = std::byte{value.dispatch};
	detail::store32(output, 8, value.session);
	detail::store32(output, 12, value.requestId);
	detail::store32(output, 16, value.endpoint);
	detail::store32(output, 20, value.payloadBytes);
	detail::store32(output, 24, value.expectedResponseBytes);
	output[28] = std::byte{value.endpointStatus};
	output[29] = output[30] = output[31] = std::byte{0};
	return Status::Ok;
}

// Copy the complete payload before the header so payload/output aliasing is
// supported. No byte is written on validation or capacity failure.
[[nodiscard]] inline BuildResult write(std::span<std::byte> output, Header header,
                                       std::span<const std::byte> payload = {}) noexcept
{
	if (payload.size() > UINT32_MAX)
		return {Status::InvalidLength};
	header.payloadBytes = static_cast<std::uint32_t>(payload.size());
	const auto status = validate(header);
	if (status != Status::Ok)
		return {status};
	if (output.size() < headerBytes || output.size() - headerBytes < payload.size())
		return {Status::BufferTooSmall};
	if (!payload.empty())
		std::memmove(output.data() + headerBytes, payload.data(), payload.size());
	(void)writeHeader(output, header);
	return {Status::Ok, headerBytes + payload.size()};
}

} // namespace telemetry::protocol::encoded
#endif // TELEMETRY_PROTOCOL_ENCODED_WIRE_HPP

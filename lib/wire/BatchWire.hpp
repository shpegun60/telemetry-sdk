/*
 * @file BatchWire.hpp
 * @brief Optional bounded mixed Encoded batch bodies and admission capability.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_PROTOCOL_BATCH_WIRE_HPP
#define TELEMETRY_PROTOCOL_BATCH_WIRE_HPP
#pragma once

#include "EncodedWire.hpp"
#include <array>

namespace telemetry::protocol::encoded::batch {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
inline constexpr std::size_t maxItems = 32;
inline constexpr std::size_t maxRequestPayload = 65535, maxReplyPayload = 65535;
inline constexpr std::size_t capabilityBytes = 24;
inline constexpr std::uint32_t capabilityMagic = 0x314d4254u; // TBM1.

// Independently admitted from read-only TRM1. The application binds this record
// to the immutable schema and session. EncodedProfile serves it as
// /telemetry/batch.bin, where a ConnectionSession looks for it
// (doc/DeviceFileConventions.md).
struct Capability {
	std::uint32_t session = 0;
	std::uint64_t fingerprint = 0;
	std::uint8_t itemLimit = maxItems;
	std::uint16_t requestLimit = maxRequestPayload, replyLimit = maxReplyPayload;
	friend bool operator==(const Capability&, const Capability&) = default;
};

[[nodiscard]] constexpr bool valid(const Capability& capability) noexcept
{
	return capability.session != 0 && capability.itemLimit != 0 &&
	       capability.itemLimit <= maxItems && capability.requestLimit >= 10 &&
	       capability.replyLimit >= 14;
}

struct CapabilityResult {
	Status status = Status::InvalidHeader;
	Capability value{};

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

// Items borrow complete canonical request values. Repeated IDs are deliberate:
// a write followed by a read, or two commands, remain ordered distinct items.
struct RequestItem {
	Operation operation = Operation::FieldRead;
	std::uint32_t id = 0;
	Input payload{};
};

struct RequestResult {
	Status status = Status::InvalidHeader;
	std::array<RequestItem, maxItems> items{};
	std::uint16_t count = 0;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

struct ItemView {
	std::uint32_t id = 0;
	Operation operation = Operation::FieldRead;
	std::uint8_t dispatch = 0;
	std::uint8_t endpointStatus = 0;
	Input payload{};
};

struct ReplyResult {
	Status status = Status::InvalidHeader;
	std::array<ItemView, maxItems> items{};
	std::uint16_t count = 0;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

namespace detail {
template<class T>
[[nodiscard]] constexpr T load(Input input, std::size_t offset) noexcept
{
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i)
		value |= std::uint64_t{std::to_integer<std::uint8_t>(input[offset + i])} << (8 * i);
	return static_cast<T>(value);
}

template<class T>
constexpr void store(Output output, std::size_t offset, T value) noexcept
{
	for (std::size_t i = 0; i < sizeof(T); ++i)
		output[offset + i] = std::byte((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xffu);
}

[[nodiscard]] inline bool overlap(Input a, Input b) noexcept
{
	if (a.empty() || b.empty())
		return false;
	const auto first = reinterpret_cast<std::uintptr_t>(a.data());
	const auto second = reinterpret_cast<std::uintptr_t>(b.data());
	return first <= second ? second - first < a.size() : first - second < b.size();
}

[[nodiscard]] constexpr bool operationValid(Operation operation) noexcept
{
	const auto value = static_cast<unsigned>(operation);
	return value >= 1 && value <= 4;
}

[[nodiscard]] inline bool itemsValid(std::span<const RequestItem> items,
                                     std::size_t limit = maxItems) noexcept
{
	if (limit == 0 || limit > maxItems || items.empty() || items.size() > limit)
		return false;
	for (const auto& item : items)
		if (!operationValid(item.operation) || item.payload.size() > maxRequestPayload ||
		    (item.operation == Operation::FieldRead && !item.payload.empty()))
			return false;
	return true;
}

[[nodiscard]] constexpr bool resultValid(Operation operation, std::uint8_t dispatch,
                                         std::uint8_t endpoint, std::size_t length) noexcept
{
	if (!operationValid(operation) ||
	    !::telemetry::protocol::encoded::detail::validDispatch(dispatch))
		return false;
	if (dispatch != 0)
		return endpoint == 0 && length == 0;
	switch (operation) {
		case Operation::FieldWrite:
			return endpoint <= 5 && length == 0;
		case Operation::Command:
			return endpoint <= 7 && length == 0;
		case Operation::Service:
			return endpoint <= 4 && (endpoint == 0 || length == 0);
		case Operation::FieldRead:
			return endpoint == 0;
		default:
			return false;
	}
}
} // namespace detail

[[nodiscard]] inline bool hasMutations(std::span<const RequestItem> items) noexcept
{
	for (const auto& item : items)
		if (item.operation != Operation::FieldRead)
			return true;
	return false;
}

[[nodiscard]] inline CapabilityResult parseCapability(Input input) noexcept
{
	if (input.size() != capabilityBytes)
		return {Status::InvalidLength};
	if (detail::load<std::uint32_t>(input, 0) != capabilityMagic || input[4] != std::byte{1} ||
	    input[6] != std::byte{1} || input[7] != std::byte{1})
		return {};
	const Capability value{
	    detail::load<std::uint32_t>(input, 12), detail::load<std::uint64_t>(input, 16),
	    std::to_integer<std::uint8_t>(input[5]), detail::load<std::uint16_t>(input, 8),
	    detail::load<std::uint16_t>(input, 10)};
	return valid(value) ? CapabilityResult{Status::Ok, value} : CapabilityResult{};
}

[[nodiscard]] inline BuildResult writeCapability(Output output, Capability value) noexcept
{
	if (!valid(value))
		return {Status::InvalidHeader};
	if (output.size() < capabilityBytes)
		return {Status::BufferTooSmall};
	detail::store(output, 0, capabilityMagic);
	output[4] = output[6] = output[7] = std::byte{1};
	output[5] = std::byte{value.itemLimit};
	detail::store(output, 8, value.requestLimit);
	detail::store(output, 10, value.replyLimit);
	detail::store(output, 12, value.session);
	detail::store(output, 16, value.fingerprint);
	return {Status::Ok, capabilityBytes};
}

[[nodiscard]] inline RequestResult parseRequest(Input input,
                                                std::size_t itemLimit = maxItems) noexcept
{
	if (input.size() < 2)
		return {Status::Truncated};
	if (input.size() > maxRequestPayload)
		return {Status::InvalidLength};
	const auto count = detail::load<std::uint16_t>(input, 0);
	if (itemLimit == 0 || itemLimit > maxItems || count == 0 || count > itemLimit)
		return {Status::InvalidHeader};
	RequestResult result{Status::Ok, {}, count};
	std::size_t cursor = 2;
	for (std::size_t i = 0; i < count; ++i) {
		if (input.size() - cursor < 8)
			return {Status::Truncated};
		const auto operation = static_cast<Operation>(std::to_integer<unsigned>(input[cursor + 4]));
		const auto length = detail::load<std::uint16_t>(input, cursor + 6);
		if (!detail::operationValid(operation) || input[cursor + 5] != std::byte{0} ||
		    (operation == Operation::FieldRead && length != 0))
			return {Status::InvalidHeader};
		const auto id = detail::load<std::uint32_t>(input, cursor);
		cursor += 8;
		if (length > input.size() - cursor)
			return {Status::Truncated};
		result.items[i] = {operation, id, input.subspan(cursor, length)};
		cursor += length;
	}
	return cursor == input.size() ? result : RequestResult{Status::InvalidLength};
}

// Every input and capacity is checked before writing the first output byte.
[[nodiscard]] inline BuildResult writeRequest(Output output, std::span<const RequestItem> items,
                                              std::size_t itemLimit = maxItems) noexcept
{
	if (!detail::itemsValid(items, itemLimit))
		return {Status::InvalidHeader};
	std::size_t needed = 2 + 8 * items.size();
	for (const auto& item : items) {
		if (item.payload.size() > maxRequestPayload - needed)
			return {Status::InvalidLength};
		needed += item.payload.size();
	}
	if (output.size() < needed)
		return {Status::BufferTooSmall};
	const auto destination = output.first(needed);
	if (detail::overlap(destination, std::as_bytes(items)))
		return {Status::InvalidHeader};
	for (const auto& item : items)
		if (detail::overlap(destination, item.payload))
			return {Status::InvalidHeader};
	detail::store(output, 0, static_cast<std::uint16_t>(items.size()));
	std::size_t cursor = 2;
	for (const auto& item : items) {
		detail::store(output, cursor, item.id);
		output[cursor + 4] = std::byte(static_cast<std::uint8_t>(item.operation));
		output[cursor + 5] = std::byte{0};
		detail::store(output, cursor + 6, static_cast<std::uint16_t>(item.payload.size()));
		cursor += 8;
		if (!item.payload.empty())
			std::memcpy(output.data() + cursor, item.payload.data(), item.payload.size());
		cursor += item.payload.size();
	}
	return {Status::Ok, needed};
}

// No borrowed item views escape on any malformed record or trailing byte.
// Exact DTO lengths and canonical value bits are checked by the schema client.
[[nodiscard]] inline ReplyResult parseReply(Input input,
                                            std::span<const RequestItem> expected) noexcept
{
	if (!detail::itemsValid(expected))
		return {Status::InvalidHeader};
	if (input.size() < 2)
		return {Status::Truncated};
	if (input.size() > maxReplyPayload)
		return {Status::InvalidLength};
	const auto count = detail::load<std::uint16_t>(input, 0);
	if (count != expected.size())
		return {Status::InvalidHeader};
	ReplyResult result{Status::Ok, {}, count};
	std::size_t cursor = 2;
	for (std::size_t i = 0; i < count; ++i) {
		if (input.size() - cursor < 12)
			return {Status::Truncated};
		const auto id = detail::load<std::uint32_t>(input, cursor);
		const auto operation = static_cast<Operation>(std::to_integer<unsigned>(input[cursor + 4]));
		const auto dispatch = std::to_integer<std::uint8_t>(input[cursor + 5]);
		const auto endpoint = std::to_integer<std::uint8_t>(input[cursor + 6]);
		const auto length = detail::load<std::uint16_t>(input, cursor + 8);
		if (id != expected[i].id || operation != expected[i].operation ||
		    input[cursor + 7] != std::byte{0} || input[cursor + 10] != std::byte{0} ||
		    input[cursor + 11] != std::byte{0} ||
		    !detail::resultValid(operation, dispatch, endpoint, length))
			return {Status::InvalidHeader};
		cursor += 12;
		if (length > input.size() - cursor)
			return {Status::Truncated};
		result.items[i] = {id, operation, dispatch, endpoint, input.subspan(cursor, length)};
		cursor += length;
	}
	return cursor == input.size() ? result : ReplyResult{Status::InvalidLength};
}
} // namespace telemetry::protocol::encoded::batch
#endif // TELEMETRY_PROTOCOL_BATCH_WIRE_HPP

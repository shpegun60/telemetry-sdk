/*
 * @file ReadManyWire.hpp
 * @brief Bounded optional Encoded ReadMany capability and canonical batch bodies.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_PROTOCOL_READ_MANY_WIRE_HPP
#define TELEMETRY_PROTOCOL_READ_MANY_WIRE_HPP
#pragma once

#include "EncodedWire.hpp"
#include <array>

namespace telemetry::protocol::encoded::read_many {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
inline constexpr std::size_t maxItems = 32, maxValueBytes = 8;
inline constexpr std::size_t maxRequestPayload = 130, maxReplyPayload = 514;
inline constexpr std::size_t capabilityBytes = 24;
inline constexpr std::uint32_t capabilityMagic = 0x314d5254u; // TRM1, not the prototype RCP0.

enum class ItemStatus : std::uint8_t {
	Ok,
	NotFound,
	UnsupportedType,
	Unavailable,
	ReadError
};

// The application discovers this record and binds it to its admitted Model and
// session. No Resource tag is added. The TCP profiles serve it as
// /telemetry/read-many.bin, where a ConnectionSession looks for it
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
	       capability.itemLimit <= maxItems && capability.requestLimit >= 6 &&
	       capability.requestLimit <= maxRequestPayload && capability.replyLimit >= 10 &&
	       capability.replyLimit <= maxReplyPayload;
}

struct CapabilityResult {
	Status status = Status::InvalidHeader;
	Capability value{};

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

struct RequestResult {
	Status status = Status::InvalidHeader;
	std::array<std::uint32_t, maxItems> ids{};
	std::uint16_t count = 0;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

struct ItemView {
	std::uint32_t id = 0;
	ItemStatus status = ItemStatus::ReadError;
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
[[nodiscard]] constexpr T load(Input bytes, std::size_t offset) noexcept
{
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i)
		value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes[offset + i])} << (8 * i);
	return static_cast<T>(value);
}

template<class T>
constexpr void store(Output bytes, std::size_t offset, T value) noexcept
{
	for (std::size_t i = 0; i < sizeof(T); ++i)
		bytes[offset + i] = std::byte((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xffu);
}

[[nodiscard]] inline bool overlap(Input first, Input second) noexcept
{
	if (first.empty() || second.empty())
		return false;
	const auto a = reinterpret_cast<std::uintptr_t>(first.data());
	const auto b = reinterpret_cast<std::uintptr_t>(second.data());
	return a <= b ? b - a < first.size() : a - b < second.size();
}

[[nodiscard]] inline bool validIds(std::span<const std::uint32_t> ids,
                                   std::size_t limit = maxItems) noexcept
{
	if (limit == 0 || limit > maxItems || ids.empty() || ids.size() > limit)
		return false;
	for (std::size_t i = 0; i < ids.size(); ++i)
		for (std::size_t j = 0; j < i; ++j)
			if (ids[i] == ids[j])
				return false;
	return true;
}
} // namespace detail

[[nodiscard]] inline CapabilityResult parseCapability(Input input) noexcept
{
	if (input.size() != capabilityBytes)
		return {Status::InvalidLength};
	if (detail::load<std::uint32_t>(input, 0) != capabilityMagic || input[4] != std::byte{1} ||
	    input[6] != std::byte{1} || input[7] != std::byte{1})
		return {};
	const Capability capability{
	    detail::load<std::uint32_t>(input, 12), detail::load<std::uint64_t>(input, 16),
	    std::to_integer<std::uint8_t>(input[5]), detail::load<std::uint16_t>(input, 8),
	    detail::load<std::uint16_t>(input, 10)};
	return valid(capability) ? CapabilityResult{Status::Ok, capability} : CapabilityResult{};
}

// On failure nothing is written. The copied record permits unaligned output.
[[nodiscard]] inline BuildResult writeCapability(Output output, Capability capability) noexcept
{
	if (!valid(capability))
		return {Status::InvalidHeader};
	if (output.size() < capabilityBytes)
		return {Status::BufferTooSmall};
	detail::store(output, 0, capabilityMagic);
	output[4] = std::byte{1};
	output[5] = std::byte{capability.itemLimit};
	output[6] = std::byte{1}; // Complete-frame Encoded v1 mapping.
	output[7] = std::byte{1}; // Read-only scalar/open-enum profile, <= 8 bytes.
	detail::store(output, 8, capability.requestLimit);
	detail::store(output, 10, capability.replyLimit);
	detail::store(output, 12, capability.session);
	detail::store(output, 16, capability.fingerprint);
	return {Status::Ok, capabilityBytes};
}

[[nodiscard]] inline RequestResult parseRequest(Input input,
                                                std::size_t itemLimit = maxItems) noexcept
{
	if (input.size() < 2)
		return {Status::Truncated};
	const auto count = detail::load<std::uint16_t>(input, 0);
	if (itemLimit == 0 || itemLimit > maxItems || count == 0 || count > itemLimit)
		return {Status::InvalidHeader};
	if (input.size() != 2 + 4 * std::size_t{count})
		return {Status::InvalidLength};
	RequestResult result{Status::Ok, {}, count};
	for (std::size_t i = 0; i < count; ++i)
		result.ids[i] = detail::load<std::uint32_t>(input, 2 + 4 * i);
	return detail::validIds(std::span{result.ids}.first(count), itemLimit)
	           ? result
	           : RequestResult{Status::InvalidHeader};
}

[[nodiscard]] inline BuildResult writeRequest(Output output, std::span<const std::uint32_t> ids,
                                              std::size_t itemLimit = maxItems) noexcept
{
	if (!detail::validIds(ids, itemLimit))
		return {Status::InvalidHeader};
	const auto needed = 2 + 4 * ids.size();
	if (output.size() < needed)
		return {Status::BufferTooSmall};
	if (detail::overlap(output.first(needed), std::as_bytes(ids)))
		return {Status::InvalidHeader};
	detail::store(output, 0, static_cast<std::uint16_t>(ids.size()));
	for (std::size_t i = 0; i < ids.size(); ++i)
		detail::store(output, 2 + 4 * i, ids[i]);
	return {Status::Ok, needed};
}

// Failure returns no partially validated spans. Successful spans borrow the
// immutable reply; the runtime client additionally checks captured types/bits.
[[nodiscard]] inline ReplyResult parseReply(Input input,
                                            std::span<const std::uint32_t> expectedIds) noexcept
{
	if (!detail::validIds(expectedIds))
		return {Status::InvalidHeader};
	if (input.size() < 2)
		return {Status::Truncated};
	const auto count = detail::load<std::uint16_t>(input, 0);
	if (count != expectedIds.size())
		return {Status::InvalidHeader};
	ReplyResult result{Status::Ok, {}, count};
	std::size_t cursor = 2;
	for (std::size_t i = 0; i < count; ++i) {
		if (input.size() - cursor < 8)
			return {Status::Truncated};
		const auto id = detail::load<std::uint32_t>(input, cursor);
		const auto status = std::to_integer<unsigned>(input[cursor + 4]);
		const auto length = detail::load<std::uint16_t>(input, cursor + 6);
		if (id != expectedIds[i] || status > static_cast<unsigned>(ItemStatus::ReadError) ||
		    input[cursor + 5] != std::byte{0} ||
		    (status == 0 ? (length != 1 && length != 2 && length != 4 && length != 8)
		                 : length != 0))
			return {Status::InvalidHeader};
		cursor += 8;
		if (length > input.size() - cursor)
			return {Status::Truncated};
		result.items[i] = {id, static_cast<ItemStatus>(status), input.subspan(cursor, length)};
		cursor += length;
	}
	return cursor == input.size() ? result : ReplyResult{Status::InvalidLength};
}
} // namespace telemetry::protocol::encoded::read_many
#endif // TELEMETRY_PROTOCOL_READ_MANY_WIRE_HPP

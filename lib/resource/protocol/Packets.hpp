/**
 * @file Packets.hpp
 * @brief Bounded request builders and checked, borrowed resource replies.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see ../LICENSE.
 *
 * Builders write only into caller-owned output. Parsers consume one complete
 * reply and borrow its data: the packet must remain alive and unchanged while
 * any parsed path, data span or iterator is used. No framing or retry state.
 */
#ifndef TELEMETRY_LIB_RESOURCE_PROTOCOL_CLIENT_HPP
#define TELEMETRY_LIB_RESOURCE_PROTOCOL_CLIENT_HPP
#pragma once

#include "Wire.hpp"
#include <resource/Types.hpp>
#include <cstring>
#include <iterator>
#include <string_view>

namespace resource::protocol::client {
enum class BuildStatus : std::uint8_t {
	Ok,
	BufferTooSmall,
	PayloadTooLarge
};

struct BuildResult {
	BuildStatus status;
	std::size_t written;
};

// Parsing errors are distinct from resource/provider statuses. A valid remote
// InvalidData response has parsing==Ok; malformed bytes have parsing==Malformed.
enum class ParseStatus : std::uint8_t {
	Ok,
	Malformed
};

namespace detail {
template<class T>
constexpr T load(Input bytes, std::size_t offset) noexcept
{
	std::uint64_t word = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		word |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
	}
	return static_cast<T>(word);
}

template<class T>
constexpr void store(Output bytes, std::size_t offset, T value) noexcept
{
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		bytes[offset + i] = std::byte((std::uint64_t(value) >> (8 * i)) & 0xffu);
	}
}

constexpr bool validStatus(Input bytes) noexcept
{
	return std::to_integer<unsigned>(bytes[0]) <= static_cast<unsigned>(Status::InternalError);
}

inline std::string_view pathView(Input path) noexcept
{
	return {reinterpret_cast<const char*>(path.data()), path.size()};
}

// Match the resource core's flat-label rules without depending on providers.
inline bool validPath(Input bytes) noexcept
{
	const auto path = pathView(bytes);
	if (path.size() < 2 || path.size() > wire::maxListPathSize || path.front() != '/' ||
	    path.back() == '/') {
		return false;
	}
	std::size_t start = 1;
	for (std::size_t i = 1; i <= path.size(); ++i) {
		if (i != path.size() && path[i] != '/') {
			const auto byte = static_cast<unsigned char>(path[i]);
			if (byte < 0x20 || byte == 0x7f || path[i] == '\\') {
				return false;
			}
			continue;
		}
		const auto part = path.substr(start, i - start);
		if (part.empty() || part == "." || part == "..") {
			return false;
		}
		start = i + 1;
	}
	return true;
}

struct PathAccess;
} // namespace detail

// Only successful parseList constructs a nonempty range. Each string_view
// borrows its path bytes from the original response; no terminators are added.
// Public methods:
// - ListPaths(): Create an empty path range.
// - begin()/end(): Iterate validated paths.
// - size()/empty(): Inspect the path count.
class ListPaths {
public:
	class Iterator {
	public:
		using value_type = std::string_view;
		using difference_type = std::ptrdiff_t;
		using iterator_concept = std::input_iterator_tag;
		using iterator_category = std::input_iterator_tag;

		constexpr Iterator() noexcept = default;

		std::string_view operator*() const noexcept
		{
			return detail::pathView(
			    remaining_.subspan(2, detail::load<std::uint16_t>(remaining_, 0)));
		}

		constexpr Iterator& operator++() noexcept
		{
			remaining_ = remaining_.subspan(2 + detail::load<std::uint16_t>(remaining_, 0));
			return *this;
		}

		constexpr Iterator operator++(int) noexcept
		{
			const auto previous = *this;
			++*this;
			return previous;
		}

		constexpr bool operator==(std::default_sentinel_t) const noexcept
		{
			return remaining_.empty();
		}

	private:
		friend class ListPaths;

		constexpr explicit Iterator(Input bytes) noexcept : remaining_(bytes)
		{}

		Input remaining_{};
	};

	constexpr ListPaths() noexcept = default;

	constexpr Iterator begin() const noexcept
	{
		return Iterator(bytes_);
	}

	constexpr std::default_sentinel_t end() const noexcept
	{
		return {};
	}

	constexpr std::size_t size() const noexcept
	{
		return count_;
	}

	constexpr bool empty() const noexcept
	{
		return count_ == 0;
	}

private:
	friend struct detail::PathAccess;

	constexpr ListPaths(Input bytes, std::size_t count) noexcept : bytes_(bytes), count_(count)
	{}

	Input bytes_{};
	std::size_t count_ = 0;
};

struct ListReply {
	Status status = Status::InvalidData;
	Cursor next = 0;
	bool eof = false;
	ListPaths paths{};
};

struct StatReply {
	Status status = Status::InvalidData;
	FileSize size = 0;
	FileFlags flags = FileFlag::None;
};

struct ReadReply {
	Status status = Status::InvalidData;
	Cursor next = 0;
	bool eof = false;
	Input data{};
};

struct WriteReply {
	Status status = Status::InvalidData;
	Cursor next = 0;
	std::uint32_t consumed = 0;
	bool complete = false;
};

struct SaveReply {
	Status status = Status::InvalidData;
	Cursor next = 0;
	bool complete = false;
};

template<class Response>
struct ParseResult {
	ParseStatus parsing = ParseStatus::Malformed;
	Response response{};
	// The only short reply is one InvalidData byte for a malformed request.
	// It has no cursor, count, flags or completion fields to interpret.
	bool shortError = false;

	constexpr explicit operator bool() const noexcept
	{
		return parsing == ParseStatus::Ok;
	}
};

namespace detail {
struct PathAccess {
	static constexpr ListPaths fromValidated(Input bytes, std::size_t count) noexcept
	{
		return {bytes, count};
	}
};

template<class Response>
constexpr ParseResult<Response> shortReply(Input bytes) noexcept
{
	if (bytes.size() == 1 && bytes[0] == std::byte(Status::InvalidData)) {
		return {ParseStatus::Ok, Response{}, true};
	}
	return {};
}
} // namespace detail

// Array and fixed/dynamic span outputs all use this same checked result.
// Failure writes no bytes. Send only the output prefix of length written.
[[nodiscard]] constexpr BuildResult makeList(Output output, Cursor cursor) noexcept
{
	if (output.size() < wire::listRequestSize) {
		return {BuildStatus::BufferTooSmall, 0};
	}
	output[0] = std::byte(Op::List);
	detail::store(output, 1, cursor);
	return {BuildStatus::Ok, wire::listRequestSize};
}

[[nodiscard]] constexpr BuildResult makeStat(Output output, FileIndex index) noexcept
{
	if (output.size() < wire::statRequestSize) {
		return {BuildStatus::BufferTooSmall, 0};
	}
	output[0] = std::byte(Op::Stat);
	detail::store(output, 1, index);
	return {BuildStatus::Ok, wire::statRequestSize};
}

[[nodiscard]] constexpr BuildResult makeRead(Output output, FileIndex index, Cursor cursor) noexcept
{
	if (output.size() < wire::readRequestSize) {
		return {BuildStatus::BufferTooSmall, 0};
	}
	output[0] = std::byte(Op::Read);
	for (std::size_t i = 0; i != sizeof(index); ++i) {
		output[1 + i] = std::byte((std::uint64_t(index) >> (8 * i)) & 0xffu);
	}
	for (std::size_t i = 0; i != sizeof(cursor); ++i) {
		output[5 + i] = std::byte((cursor >> (8 * i)) & 0xffu);
	}
	return {BuildStatus::Ok, wire::readRequestSize};
}

// Move payload before writing the header, so input may overlap output, even
// when the input bytes occupy the header's future position.
[[nodiscard]] inline BuildResult makeWrite(Output output, FileIndex index, Cursor cursor,
                                           Input input, bool final) noexcept
{
	if (input.size() > wire::maxPayloadSize) {
		return {BuildStatus::PayloadTooLarge, 0};
	}
	const auto required = wire::writeRequestHeaderSize + input.size();
	if (output.size() < required) {
		return {BuildStatus::BufferTooSmall, 0};
	}
	if (!input.empty()) {
		std::memmove(output.data() + wire::writeRequestHeaderSize, input.data(), input.size());
	}
	output[0] = std::byte(Op::Write);
	detail::store(output, 1, index);
	detail::store(output, 5, cursor);
	output[13] = std::byte(final);
	detail::store(output, 14, static_cast<std::uint16_t>(input.size()));
	return {BuildStatus::Ok, required};
}

[[nodiscard]] inline ParseResult<ReadReply> parseRead(Input bytes) noexcept
{
	if (bytes.size() == 1) {
		return detail::shortReply<ReadReply>(bytes);
	}
	if (bytes.size() < wire::chunkReplyHeaderSize || !detail::validStatus(bytes) ||
	    std::to_integer<unsigned>(bytes[9]) > 1) {
		return {};
	}
	const auto count = detail::load<std::uint16_t>(bytes, 10);
	if (bytes.size() - wire::chunkReplyHeaderSize != count) {
		return {};
	}
	const auto status = static_cast<Status>(std::to_integer<unsigned>(bytes[0]));
	const auto eof = bytes[9] != std::byte{};
	if (status != Status::Ok && (count != 0 || eof)) {
		return {};
	}
	return {
	    ParseStatus::Ok,
	    {status, detail::load<Cursor>(bytes, 1), eof, bytes.subspan(wire::chunkReplyHeaderSize)},
	    false};
}

[[nodiscard]] inline ParseResult<ListReply> parseList(Input bytes) noexcept
{
	const auto chunk = parseRead(bytes);
	if (!chunk) {
		return {};
	}
	if (chunk.shortError) {
		return detail::shortReply<ListReply>(bytes);
	}
	const auto data = chunk.response.data;
	std::size_t offset = 0;
	std::size_t count = 0;
	while (offset != data.size()) {
		if (data.size() - offset < 2) {
			return {};
		}
		const auto length = detail::load<std::uint16_t>(data, offset);
		offset += 2;
		if (length > data.size() - offset || !detail::validPath(data.subspan(offset, length))) {
			return {};
		}
		offset += length;
		++count;
	}
	// LIST's cursor is an index, so it cannot precede the number of paths.
	if (chunk.response.next < count) {
		return {};
	}
	return {ParseStatus::Ok,
	        {chunk.response.status, chunk.response.next, chunk.response.eof,
	         detail::PathAccess::fromValidated(data, count)},
	        false};
}

[[nodiscard]] constexpr ParseResult<StatReply> parseStat(Input bytes) noexcept
{
	if (bytes.size() == 1) {
		return detail::shortReply<StatReply>(bytes);
	}
	// Flag bits this client does not know are kept and ignored by has(), so a
	// flag a later server adds does not turn its STAT into a malformed reply.
	if (bytes.size() != wire::statReplySize || !detail::validStatus(bytes)) {
		return {};
	}
	return {ParseStatus::Ok,
	        {static_cast<Status>(std::to_integer<unsigned>(bytes[0])),
	         detail::load<FileSize>(bytes, 1),
	         static_cast<FileFlags>(std::to_integer<unsigned>(bytes[5]))},
	        false};
}

// submittedBytes is the payload length of the WRITE request being answered.
// Providers may advance opaque cursors independently of consumed byte counts.
[[nodiscard]] constexpr ParseResult<WriteReply> parseWrite(Input bytes,
                                                           std::size_t submittedBytes) noexcept
{
	if (submittedBytes > wire::maxPayloadSize) {
		return {};
	}
	if (bytes.size() == 1) {
		return detail::shortReply<WriteReply>(bytes);
	}
	if (bytes.size() != wire::writeReplySize || !detail::validStatus(bytes) ||
	    std::to_integer<unsigned>(bytes[13]) > 1) {
		return {};
	}
	const auto consumed = detail::load<std::uint32_t>(bytes, 9);
	const auto status = static_cast<Status>(std::to_integer<unsigned>(bytes[0]));
	const auto complete = bytes[13] != std::byte{};
	if (consumed > submittedBytes || (status != Status::Ok && (consumed != 0 || complete))) {
		return {};
	}
	return {ParseStatus::Ok, {status, detail::load<Cursor>(bytes, 1), consumed, complete}, false};
}

// SAVE: persist what the file at index holds now, in steps from cursor 0 until
// the reply is complete.
[[nodiscard]] constexpr BuildResult makeSave(Output output, FileIndex index, Cursor cursor) noexcept
{
	if (output.size() < wire::saveRequestSize) {
		return {BuildStatus::BufferTooSmall, 0};
	}
	output[0] = std::byte(Op::Save);
	for (std::size_t i = 0; i != sizeof(index); ++i) {
		output[1 + i] = std::byte((std::uint64_t(index) >> (8 * i)) & 0xffu);
	}
	for (std::size_t i = 0; i != sizeof(cursor); ++i) {
		output[5 + i] = std::byte((cursor >> (8 * i)) & 0xffu);
	}
	return {BuildStatus::Ok, wire::saveRequestSize};
}

[[nodiscard]] constexpr ParseResult<SaveReply> parseSave(Input bytes) noexcept
{
	if (bytes.size() == 1) {
		return detail::shortReply<SaveReply>(bytes);
	}
	if (bytes.size() != wire::saveReplySize || !detail::validStatus(bytes) ||
	    std::to_integer<unsigned>(bytes[9]) > 1) {
		return {};
	}
	const auto status = static_cast<Status>(std::to_integer<unsigned>(bytes[0]));
	const auto complete = bytes[9] != std::byte{};
	if (status != Status::Ok && complete) {
		return {};
	}
	return {ParseStatus::Ok, {status, detail::load<Cursor>(bytes, 1), complete}, false};
}
} // namespace resource::protocol::client

#endif // TELEMETRY_LIB_RESOURCE_PROTOCOL_CLIENT_HPP

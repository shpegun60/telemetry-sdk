/**
 * @file Protocol.cpp
 * @brief Explicit byte encoding and whole-entry LIST pagination.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see ../LICENSE.
 */
#include "Protocol.hpp"
#include <algorithm>
#include <cstring>

namespace resource::protocol {
namespace {
using namespace resource;
constexpr std::size_t chunkHeader = wire::chunkReplyHeaderSize;
constexpr std::size_t statHeader = wire::statReplySize;
constexpr std::size_t writeHeader = wire::writeReplySize;
constexpr std::size_t saveHeader = wire::saveReplySize;
constexpr std::size_t maxPayload = wire::maxPayloadSize;

// These helpers are used only after checking the complete fixed header.
template<class T>
T load(Input bytes, std::size_t offset) noexcept
{
	std::uint64_t word = 0;
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		word |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
	}
	return static_cast<T>(word);
}

template<class T>
void store(Output bytes, std::size_t offset, T value) noexcept
{
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		bytes[offset + i] = std::byte((std::uint64_t(value) >> (8 * i)) & 0xffu);
	}
}

Reply shortError(Status status, Output out) noexcept
{
	if (out.empty()) {
		return {status, 0};
	}
	out[0] = std::byte(status);
	return {status, 1};
}

Reply chunkReply(Output out, Status status, Cursor cursor, std::size_t count, bool eof) noexcept
{
	out[0] = std::byte(status);
	store(out, 1, cursor);
	out[9] = std::byte(eof);
	store(out, 10, static_cast<std::uint16_t>(count));
	return {status, chunkHeader + count};
}

bool validStatus(Status status) noexcept
{
	return static_cast<unsigned>(status) <= static_cast<unsigned>(Status::InternalError);
}
} // namespace

Reply process(resource::FileSystemView files, resource::Input request,
              resource::Output response) noexcept
{
	using namespace resource;
	if (request.empty()) {
		return shortError(Status::InvalidData, response);
	}
	const auto op = static_cast<Op>(std::to_integer<unsigned char>(request[0]));
	if (op == Op::List) {
		// LIST owns its index cursor here. Resource providers never see it.
		if (request.size() != wire::listRequestSize) {
			return shortError(Status::InvalidData, response);
		}
		const auto cursor = load<Cursor>(request, 1);
		if (response.size() < chunkHeader) {
			return {Status::BufferTooSmall, 0};
		}
		if (cursor > files.fileCount()) {
			return chunkReply(response, Status::InvalidCursor, cursor, 0, false);
		}
		// Reserve the envelope first; its dataSize also caps a large buffer.
		const auto capacity = std::min(response.size() - chunkHeader, maxPayload);
		std::size_t used = 0;
		auto next = cursor;
		while (next < files.fileCount()) {
			const auto path = files.path(static_cast<FileIndex>(next));
			// The u16 dataSize includes this entry's u16 length prefix too.
			// A larger path can never fit, even with an unlimited caller buffer.
			if (path.size() > wire::maxListPathSize) {
				// Keep a completed prefix visible. The next request starts at
				// this unrepresentable entry and reports its error separately.
				if (used != 0) {
					break;
				}
				return chunkReply(response, Status::InvalidData, cursor, 0, false);
			}
			const auto required = 2 + path.size();
			if (required > capacity - used) {
				// Already emitted entries form a useful page. An empty page
				// cannot advance, so report that a larger buffer is required.
				if (used == 0) {
					return chunkReply(response, Status::BufferTooSmall, cursor, 0, false);
				}
				break;
			}
			store(response, chunkHeader + used, static_cast<std::uint16_t>(path.size()));
			std::memcpy(response.data() + chunkHeader + used + 2, path.data(), path.size());
			used += required;
			++next;
		}
		return chunkReply(response, Status::Ok, next, used, next == files.fileCount());
	}
	if (op == Op::Stat) {
		// STAT queries current size; LIST deliberately never makes this call.
		if (request.size() != wire::statRequestSize) {
			return shortError(Status::InvalidData, response);
		}
		if (response.size() < statHeader) {
			return {Status::BufferTooSmall, 0};
		}
		const auto result = files.stat(load<FileIndex>(request, 1));
		response[0] = std::byte(result.status);
		store(response, 1, result.size);
		response[5] = std::byte(result.flags);
		return {result.status, statHeader};
	}
	if (op == Op::Read) {
		// The provider writes directly after the response header. Validate its
		// reported count before publishing any of those bytes to the transport.
		if (request.size() != wire::readRequestSize) {
			return shortError(Status::InvalidData, response);
		}
		const auto index = load<FileIndex>(request, 1);
		const auto cursor = load<Cursor>(request, 5);
		if (response.size() < chunkHeader) {
			return {Status::BufferTooSmall, 0};
		}
		auto payload =
		    response.subspan(chunkHeader, std::min(response.size() - chunkHeader, maxPayload));
		const auto result = files.read(index, cursor, payload);
		if (!validStatus(result.status) || result.written > payload.size() ||
		    (result.status != Status::Ok &&
		     (result.written != 0 || result.next != cursor || result.eof)) ||
		    (result.status == Status::Ok && !result.eof && result.written == 0 &&
		     result.next == cursor)) {
			return chunkReply(response, Status::InternalError, cursor, 0, false);
		}
		return chunkReply(response, result.status, result.next, result.written, result.eof);
	}
	if (op == Op::Write) {
		if (request.size() < wire::writeRequestHeaderSize) {
			return shortError(Status::InvalidData, response);
		}
		const auto length = load<std::uint16_t>(request, 14);
		const auto final = std::to_integer<unsigned char>(request[13]);
		if (request.size() - wire::writeRequestHeaderSize != length || final > 1) {
			return shortError(Status::InvalidData, response);
		}
		const auto index = load<FileIndex>(request, 1);
		const auto cursor = load<Cursor>(request, 5);
		if (response.size() < writeHeader) {
			return {Status::BufferTooSmall, 0};
		}
		// Parse first, invoke second, encode last: an overlapping packet/reply
		// buffer remains valid for the synchronous provider's input lifetime.
		auto result =
		    files.write(index, cursor, request.subspan(wire::writeRequestHeaderSize), final != 0);
		if (!validStatus(result.status) || result.consumed > length ||
		    (result.status != Status::Ok &&
		     (result.consumed != 0 || result.next != cursor || result.complete)) ||
		    (result.status == Status::Ok && !result.complete && result.consumed == 0 &&
		     result.next == cursor)) {
			result = {Status::InternalError, cursor};
		}
		response[0] = std::byte(result.status);
		store(response, 1, result.next);
		store(response, 9, result.consumed);
		response[13] = std::byte(result.complete);
		return {result.status, writeHeader};
	}
	if (op == Op::Save) {
		if (request.size() != wire::saveRequestSize) {
			return shortError(Status::InvalidData, response);
		}
		const auto index = load<FileIndex>(request, 1);
		const auto cursor = load<Cursor>(request, 5);
		if (response.size() < saveHeader) {
			return {Status::BufferTooSmall, 0};
		}
		// As WRITE: an error makes no progress and is not complete; an Ok step
		// that does not complete must move the cursor.
		auto result = files.save(index, cursor);
		if (!validStatus(result.status) ||
		    (result.status != Status::Ok && (result.next != cursor || result.complete)) ||
		    (result.status == Status::Ok && !result.complete && result.next == cursor)) {
			result = {Status::InternalError, cursor};
		}
		response[0] = std::byte(result.status);
		store(response, 1, result.next);
		response[9] = std::byte(result.complete);
		return {result.status, saveHeader};
	}
	return shortError(Status::InvalidData, response);
}
} // namespace resource::protocol

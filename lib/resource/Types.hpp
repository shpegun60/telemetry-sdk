/**
 * @file Types.hpp
 * @brief Transport-independent resource results and non-owning byte ranges.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see LICENSE.
 *
 * Shared counts, capabilities and transfer results keep providers independent
 * of packet framing. Input and Output borrow storage; neither extends its
 * lifetime or supplies synchronization.
 */

#ifndef TELEMETRY_LIB_RESOURCE_TYPES_HPP
#define TELEMETRY_LIB_RESOURCE_TYPES_HPP
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace resource {
using FileIndex = std::uint32_t;
using FileSize = std::uint32_t;
using Cursor = std::uint64_t;
using Output = std::span<std::byte>;
using Input = std::span<const std::byte>;

enum class Status : std::uint8_t {
	Ok = 0,
	InvalidFile = 1,
	NotReadable = 2,
	NotWritable = 3,
	InvalidCursor = 4,
	CursorExpired = 5,
	BufferTooSmall = 6,
	InvalidData = 7,
	InternalError = 8
};
enum class FileFlag : std::uint8_t {
	None = 0,
	Readable = 1,
	Writable = 2,
	// The file persists what it holds on SAVE.
	Savable = 4
};
using FileFlags = FileFlag;

constexpr FileFlags operator|(FileFlags a, FileFlags b) noexcept
{
	return static_cast<FileFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool has(FileFlags value, FileFlag bit) noexcept
{
	return (static_cast<unsigned>(value) & static_cast<unsigned>(bit)) ==
	       static_cast<unsigned>(bit);
}

// Cursor zero starts a transfer. Only the provider interprets other cursors.
// For Ok, written/consumed never exceeds the supplied span. On core errors
// no provider is invoked, the cursor is unchanged and the count is zero.
// Store the widest members first to avoid padding in these frequent returns.
// Constructors keep the public {status, cursor, count, finished} argument order;
// that order is independent of the in-memory layout and the wire encoding.
// Public methods:
// - ReadResult(): Report read outcome.
struct ReadResult {
	Cursor next = 0;
	std::uint32_t written = 0;
	Status status = Status::Ok;
	bool eof = false;

	constexpr ReadResult(Status resultStatus = Status::Ok, Cursor nextCursor = 0,
	                     std::uint32_t byteCount = 0, bool atEnd = false) noexcept
	    : next(nextCursor), written(byteCount), status(resultStatus), eof(atEnd)
	{}
};

// Public methods:
// - WriteResult(): Report write outcome.
struct WriteResult {
	Cursor next = 0;
	std::uint32_t consumed = 0;
	Status status = Status::Ok;
	bool complete = false;

	constexpr WriteResult(Status resultStatus = Status::Ok, Cursor nextCursor = 0,
	                      std::uint32_t byteCount = 0, bool finished = false) noexcept
	    : next(nextCursor), consumed(byteCount), status(resultStatus), complete(finished)
	{}
};

// SAVE persists in steps: Ok and not complete must move the cursor; an error
// leaves the cursor and is never complete.
// Public methods:
// - SaveResult(): Report save outcome.
struct SaveResult {
	Cursor next = 0;
	Status status = Status::Ok;
	bool complete = false;

	constexpr SaveResult(Status resultStatus = Status::Ok, Cursor nextCursor = 0,
	                     bool finished = false) noexcept
	    : next(nextCursor), status(resultStatus), complete(finished)
	{}
};

// Public methods:
// - FileStat(): Report provider metadata.
struct FileStat {
	FileSize size = 0;
	Status status = Status::Ok;
	FileFlags flags = FileFlag::None;

	constexpr FileStat(Status resultStatus = Status::Ok, FileSize byteCount = 0,
	                   FileFlags capabilities = FileFlag::None) noexcept
	    : size(byteCount), status(resultStatus), flags(capabilities)
	{}
};
} // namespace resource

#endif // TELEMETRY_LIB_RESOURCE_TYPES_HPP

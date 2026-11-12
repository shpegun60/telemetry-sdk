/**
 * @file StagedFile.hpp
 * @brief Read/write resource whose new bytes take effect only when the last piece arrives.
 * @author Ruslan Kovtun (shpegun60)
 * License: MIT; see LICENSE.
 *
 * A file written in pieces over WRITE replaces its bytes only when the piece
 * marked final completes the whole file. Pieces are kept in a stage the
 * application lends; until then reads return the old bytes, and a write that
 * stops part way - cancelled, disconnected, refused - leaves them whole. A
 * new transfer (cursor 0) drops an unfinished stage. The file keeps its size:
 * a stream longer or shorter than the file is refused with InvalidData and
 * changes nothing. Cursors are byte offsets into the stage.
 *
 * An optional commit hook sees the complete new bytes before they replace the
 * old ones and may refuse them - a value out of range, an id that must not
 * change, storage that failed: any status but Ok is the WRITE's answer and
 * the old bytes stay. It runs synchronously inside the request, so slow
 * storage delays the reply against the client's timeout.
 */

#ifndef TELEMETRY_LIB_RESOURCE_STAGEDFILE_HPP
#define TELEMETRY_LIB_RESOURCE_STAGEDFILE_HPP
#pragma once

#include "Types.hpp"
#include <cstring>

namespace resource {
// bytes and stage are the application's storage, the stage at least the
// size of the bytes (a smaller one is refused with InternalError on every
// write); both must outlive the provider. onCommit (when given) is called once
// per completed write with the staged bytes, before they replace the old
// ones: Ok lets them in; any other status is the WRITE's answer and keeps the
// old bytes. It must not call back into the provider.
// Public methods:
// - StagedFile(): Borrow the file's bytes and a stage.
// - size(): Report the file's byte extent.
// - read(): Read the committed bytes.
// - write(): Stage a piece; commit on the final one.
// - staged(): Count bytes staged by an unfinished write.
class StagedFile {
public:
	using Commit = Status (*)(void* context, Input bytes) noexcept;

	constexpr StagedFile(Output bytes, Output stage, Commit onCommit = nullptr,
	                     void* context = nullptr) noexcept
	    : bytes_(bytes), stage_(stage), onCommit_(onCommit), context_(context)
	{}

	[[nodiscard]] constexpr FileSize size() const noexcept
	{
		return static_cast<FileSize>(bytes_.size());
	}

	[[nodiscard]] ReadResult read(Cursor cursor, Output output) const noexcept
	{
		const auto bytes = size();
		if (cursor > bytes) {
			return {Status::InvalidCursor, cursor};
		}
		if (cursor == bytes) {
			return {Status::Ok, cursor, 0, true};
		}
		if (output.empty()) {
			return {Status::BufferTooSmall, cursor};
		}
		const auto offset = static_cast<FileSize>(cursor);
		const auto remaining = bytes - offset;
		const auto count =
		    static_cast<FileSize>(output.size() < remaining ? output.size() : remaining);
		std::memmove(output.data(), bytes_.data() + offset, count);
		return {Status::Ok, cursor + count, count, cursor + count == bytes};
	}

	[[nodiscard]] WriteResult write(Cursor cursor, Input input, bool final) noexcept
	{
		// Cursor 0 starts a transfer; any other must continue the one staged.
		if (cursor == 0) {
			staged_ = 0;
			active_ = true;
		}
		if (!active_ || cursor != staged_) {
			return {Status::InvalidCursor, cursor};
		}
		if (stage_.size() < bytes_.size()) {
			active_ = false;
			return {Status::InternalError, cursor};
		}
		// More than the file holds: refused, nothing staged from it.
		if (input.size() > bytes_.size() - staged_) {
			active_ = false;
			return {Status::InvalidData, cursor};
		}
		if (!input.empty()) {
			std::memcpy(stage_.data() + staged_, input.data(), input.size());
		}
		staged_ += input.size();
		const auto consumed = static_cast<std::uint32_t>(input.size());
		if (!final) {
			if (consumed == 0) {
				return {Status::InvalidData, cursor};
			}
			return {Status::Ok, staged_, consumed, false};
		}
		active_ = false;
		// Shorter than the file: refused, the old bytes kept.
		if (staged_ != bytes_.size()) {
			return {Status::InvalidData, cursor};
		}
		// The hook sees the new bytes first and may keep the old ones.
		if (onCommit_) {
			const auto verdict = onCommit_(context_, Input{stage_.data(), bytes_.size()});
			if (verdict != Status::Ok) {
				return {verdict, cursor};
			}
		}
		if (!bytes_.empty()) {
			std::memcpy(bytes_.data(), stage_.data(), bytes_.size());
		}
		return {Status::Ok, staged_, consumed, true};
	}

	[[nodiscard]] constexpr Cursor staged() const noexcept
	{
		return active_ ? staged_ : 0;
	}

private:
	Output bytes_;
	Output stage_;
	Commit onCommit_ = nullptr;
	void* context_ = nullptr;
	Cursor staged_ = 0;
	bool active_ = false;
};
} // namespace resource

#endif // TELEMETRY_LIB_RESOURCE_STAGEDFILE_HPP

/**
 * @file FileView.hpp
 * @brief Borrowed file facade and forward iterator over constant descriptors.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see LICENSE.
 *
 * Carry a file identity across a transport or application boundary without
 * copying its provider. Invalid views report core errors before callbacks;
 * valid views and iterators borrow the stable descriptor table.
 */

#ifndef TELEMETRY_LIB_RESOURCE_FILEVIEW_HPP
#define TELEMETRY_LIB_RESOURCE_FILEVIEW_HPP
#pragma once

#include "File.hpp"
#include <iterator>

namespace resource {
class FileSystemView;
class FileIterator;

// Borrows one descriptor directly, never the FileSystemView that returned it.
// The descriptor table, its path and provider must outlive this facade.
// Public methods:
// - FileView(): Create invalid view.
// - index(): Report file identity.
// - valid(): Check descriptor presence.
// - operator bool(): Check descriptor presence.
// - path(): Borrow file label.
// - readable(): Inspect read capability.
// - writable(): Inspect write capability.
// - stat(): Query file metadata.
// - read(): Read provider chunk.
// - write(): Write provider chunk.
class FileView {
public:
	constexpr FileView() noexcept = default;

	constexpr FileIndex index() const noexcept
	{
		return index_;
	}

	constexpr bool valid() const noexcept
	{
		return file_ != nullptr;
	}

	constexpr explicit operator bool() const noexcept
	{
		return valid();
	}

	constexpr std::string_view path() const noexcept
	{
		return file_ ? file_->path : std::string_view{};
	}

	// Inspect declared capabilities without calling the provider.
	constexpr bool readable() const noexcept
	{
		return file_ && file_->ops->read != nullptr;
	}

	constexpr bool writable() const noexcept
	{
		return file_ && file_->ops->write != nullptr;
	}

	constexpr bool savable() const noexcept
	{
		return file_ && file_->ops->save != nullptr;
	}

	[[nodiscard]] FileStat stat() const noexcept
	{
		if (!file_) {
			return {Status::InvalidFile};
		}
		const auto flags = (file_->ops->read ? FileFlag::Readable : FileFlag::None) |
		                   (file_->ops->write ? FileFlag::Writable : FileFlag::None) |
		                   (file_->ops->save ? FileFlag::Savable : FileFlag::None);
		return {Status::Ok, file_->ops->size(file_->object), flags};
	}

	// Core refusal preserves cursor/count and never calls the provider.
	[[nodiscard]] ReadResult read(Cursor cursor, Output out) const noexcept
	{
		if (!file_) {
			return {Status::InvalidFile, cursor};
		}
		if (!file_->ops->read) {
			return {Status::NotReadable, cursor};
		}
		return file_->ops->read(file_->object, cursor, out);
	}

	// final is forwarded unchanged; completion and cursor meaning are provider policy.
	[[nodiscard]] WriteResult write(Cursor cursor, Input in, bool final = false) const noexcept
	{
		if (!file_) {
			return {Status::InvalidFile, cursor};
		}
		if (!file_->ops->write) {
			return {Status::NotWritable, cursor};
		}
		return file_->ops->write(file_->object, cursor, in, final);
	}

	// Persist what the file holds now, in steps from cursor 0; NotWritable without save().
	[[nodiscard]] SaveResult save(Cursor cursor) const noexcept
	{
		if (!file_) {
			return {Status::InvalidFile, cursor};
		}
		if (!file_->ops->save) {
			return {Status::NotWritable, cursor};
		}
		return file_->ops->save(file_->object, cursor);
	}

private:
	friend class FileSystemView;
	friend class FileIterator;

	constexpr FileView(const FileEntry* file, FileIndex index) noexcept : file_(file), index_(index)
	{}

	const FileEntry* file_ = nullptr;
	FileIndex index_ = 0;
};

// Enumeration reads descriptors only. stat/read/write are explicit operations
// on the returned FileView. Incrementing or dereferencing end is outside the
// iterator contract, as with other forward iterators.
// Public methods:
// - FileIterator(): Create default iterator.
// - operator*(): Borrow current file.
// - operator++(): Advance file position.
// - operator++(int): Advance retaining position.
// - operator==(): Compare iterator positions.
class FileIterator {
public:
	using value_type = FileView;
	using reference = FileView;
	using difference_type = std::ptrdiff_t;
	using iterator_category = std::forward_iterator_tag;
	using iterator_concept = std::forward_iterator_tag;

	constexpr FileIterator() noexcept = default;

	constexpr FileView operator*() const noexcept
	{
		return {std::addressof(files_[index_]), index_};
	}

	constexpr FileIterator& operator++() noexcept
	{
		++index_;
		return *this;
	}

	constexpr FileIterator operator++(int) noexcept
	{
		auto previous = *this;
		++*this;
		return previous;
	}

	friend constexpr bool operator==(const FileIterator&, const FileIterator&) noexcept = default;

private:
	friend class FileSystemView;

	constexpr FileIterator(const FileEntry* files, FileIndex index) noexcept
	    : files_(files), index_(index)
	{}

	const FileEntry* files_ = nullptr;
	FileIndex index_ = 0;
};
} // namespace resource

#endif // TELEMETRY_LIB_RESOURCE_FILEVIEW_HPP

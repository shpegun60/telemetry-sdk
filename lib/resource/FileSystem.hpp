/**
 * @file FileSystem.hpp
 * @brief Flat constant descriptor table with O(1) runtime indexing.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see LICENSE.
 *
 * Declare a flat table once and dispatch by declaration position at runtime.
 * The table owns descriptors but borrows their paths and providers; its
 * borrowed views and iterators require those addresses to remain stable.
 */

#ifndef TELEMETRY_LIB_RESOURCE_FILESYSTEM_HPP
#define TELEMETRY_LIB_RESOURCE_FILESYSTEM_HPP
#pragma once

#include "FileView.hpp"
#include <array>
#include <cstddef>
#include <limits>
#include <ranges>
#include <utility>

namespace resource {
template<std::size_t N>
class FileSystem;

// Small borrowed runtime facade. The table, paths and providers must remain
// alive at stable addresses. No directory parsing, allocation or telemetry.
// Public methods:
// - FileSystemView(): Create empty facade.
// - fileCount(): Count declared files.
// - size(): Count declared files.
// - empty(): Check table emptiness.
// - operator[](): Borrow checked file.
// - begin(): Borrow first position.
// - end(): Borrow end position.
// - path(): Borrow file label.
// - stat(): Query provider metadata.
// - read(): Read provider chunk.
// - write(): Write provider chunk.
class FileSystemView {
public:
	constexpr FileSystemView() noexcept = default;

	constexpr std::size_t fileCount() const noexcept
	{
		return count_;
	}

	constexpr std::size_t size() const noexcept
	{
		return count_;
	}

	constexpr bool empty() const noexcept
	{
		return count_ == 0;
	}

	// Out-of-range lookup retains the requested index in an invalid view.
	constexpr FileView operator[](FileIndex index) const noexcept
	{
		return {index < count_ ? std::addressof(files_[index]) : nullptr, index};
	}

	constexpr FileIterator begin() const noexcept
	{
		return {files_, 0};
	}

	constexpr FileIterator end() const noexcept
	{
		return {files_, static_cast<FileIndex>(count_)};
	}

	constexpr std::string_view path(FileIndex index) const noexcept
	{
		return index < count_ ? files_[index].path : std::string_view{};
	}

	[[nodiscard]] FileStat stat(FileIndex index) const noexcept
	{
		if (index >= count_) {
			return {Status::InvalidFile};
		}
		const auto& file = files_[index];
		// Capability has one source of truth: a present operation callback.
		const auto flags = (file.ops->read ? FileFlag::Readable : FileFlag::None) |
		                   (file.ops->write ? FileFlag::Writable : FileFlag::None) |
		                   (file.ops->save ? FileFlag::Savable : FileFlag::None);
		return {Status::Ok, file.ops->size(file.object), flags};
	}

	[[nodiscard]] ReadResult read(FileIndex index, Cursor cursor, Output out) const noexcept
	{
		if (index >= count_) {
			return {Status::InvalidFile, cursor};
		}
		const auto& file = files_[index];
		if (!file.ops->read) {
			return {Status::NotReadable, cursor};
		}
		return file.ops->read(file.object, cursor, out);
	}

	[[nodiscard]] WriteResult write(FileIndex index, Cursor cursor, Input in,
	                                bool final = false) const noexcept
	{
		if (index >= count_) {
			return {Status::InvalidFile, cursor};
		}
		const auto& file = files_[index];
		if (!file.ops->write) {
			return {Status::NotWritable, cursor};
		}
		return file.ops->write(file.object, cursor, in, final);
	}

	// A file without save() answers NotWritable: it has nothing to persist.
	[[nodiscard]] SaveResult save(FileIndex index, Cursor cursor) const noexcept
	{
		if (index >= count_) {
			return {Status::InvalidFile, cursor};
		}
		const auto& file = files_[index];
		if (!file.ops->save) {
			return {Status::NotWritable, cursor};
		}
		return file.ops->save(file.object, cursor);
	}

private:
	template<std::size_t>
	friend class FileSystem;

	constexpr FileSystemView(const FileEntry* files, std::size_t count) noexcept
	    : files_(files), count_(count)
	{}

	const FileEntry* files_ = nullptr;
	std::size_t count_ = 0;
};

// The table owns only descriptors. Moving/copying it does not relocate a
// provider, but existing views still borrow the original descriptor storage.
template<std::size_t N>
// Public methods:
// - FileSystem(): Own descriptor table.
// - view(): Borrow runtime facade.
// - fileCount(): Count declared files.
// - size(): Count declared files.
// - empty(): Check table emptiness.
// - operator[](): Borrow checked file.
// - begin(): Borrow first position.
// - end(): Borrow end position.
// - path(): Borrow file label.
// - stat(): Query provider metadata.
// - read(): Read provider chunk.
// - write(): Write provider chunk.
class FileSystem {
	static_assert(N <= std::numeric_limits<FileIndex>::max(), "Too many resource files");
	static_assert(std::is_standard_layout_v<FileEntry> && offsetof(FileEntry, path) == 0);
#if defined(__arm__) && defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
	// Keep the ARM32 descriptor ABI checked without exposing private storage.
	static_assert(sizeof(FileEntry) == 16 && alignof(FileEntry) == 4);
	static_assert(offsetof(FileEntry, object) == 8 && offsetof(FileEntry, ops) == 12);
#endif

public:
	constexpr explicit FileSystem(std::array<FileEntry, N> entries) noexcept : files_(entries)
	{
		// Definition validation happens once. Runtime lookup never scans paths.
		for (std::size_t i = 0; i < N; ++i) {
			for (std::size_t j = 0; j < i; ++j) {
				if (files_[i].path == files_[j].path) {
					detail::invalidDefinition();
				}
			}
		}
	}

	constexpr FileSystemView view() const& noexcept
	{
		return {files_.data(), N};
	}

	FileSystemView view() const&& = delete;

	static constexpr std::size_t fileCount() noexcept
	{
		return N;
	}

	static constexpr std::size_t size() noexcept
	{
		return N;
	}

	static constexpr bool empty() noexcept
	{
		return N == 0;
	}

	constexpr FileView operator[](FileIndex index) const& noexcept
	{
		return view()[index];
	}

	FileView operator[](FileIndex) const&& = delete;

	constexpr FileIterator begin() const& noexcept
	{
		return view().begin();
	}

	FileIterator begin() const&& = delete;

	constexpr FileIterator end() const& noexcept
	{
		return view().end();
	}

	FileIterator end() const&& = delete;

	constexpr std::string_view path(FileIndex index) const noexcept
	{
		return view().path(index);
	}

	[[nodiscard]] FileStat stat(FileIndex index) const noexcept
	{
		return view().stat(index);
	}

	[[nodiscard]] ReadResult read(FileIndex index, Cursor cursor, Output output) const noexcept
	{
		return view().read(index, cursor, output);
	}

	[[nodiscard]] WriteResult write(FileIndex index, Cursor cursor, Input input,
	                                bool final = false) const noexcept
	{
		return view().write(index, cursor, input, final);
	}

	[[nodiscard]] SaveResult save(FileIndex index, Cursor cursor) const noexcept
	{
		return view().save(index, cursor);
	}

private:
	const std::array<FileEntry, N> files_;
};

template<class... Entry>
    requires(std::same_as<Entry, FileEntry> && ...)
constexpr auto filesystem(Entry... entries) noexcept
{
	return FileSystem<sizeof...(Entry)>{std::array<FileEntry, sizeof...(Entry)>{entries...}};
}

// Files that come first - a protocol profile's own, from its systemFiles() -
// then the application's: its logs, settings and data, in the order listed.
template<std::size_t N, class... Entry>
    requires(std::same_as<Entry, FileEntry> && ...)
constexpr auto filesystem(const std::array<FileEntry, N>& first, Entry... more) noexcept
{
	return [&]<std::size_t... I>(std::index_sequence<I...>) {
		return FileSystem<N + sizeof...(Entry)>{
		    std::array<FileEntry, N + sizeof...(Entry)>{first[I]..., more...}};
	}(std::make_index_sequence<N>{});
}
} // namespace resource

// Iterators borrow the descriptor table rather than the small view object.
template<>
inline constexpr bool std::ranges::enable_borrowed_range<resource::FileSystemView> = true;

#endif // TELEMETRY_LIB_RESOURCE_FILESYSTEM_HPP

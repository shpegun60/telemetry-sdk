/**
 * @file File.hpp
 * @brief C++20 provider contracts and constexpr non-owning file bindings.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see LICENSE.
 *
 * Bind provider operations once so runtime access needs only a descriptor
 * and function pointers. Names and provider objects remain externally owned;
 * the deleted overloads prevent accidental borrowing from temporaries.
 */

#ifndef TELEMETRY_LIB_RESOURCE_FILE_HPP
#define TELEMETRY_LIB_RESOURCE_FILE_HPP
#pragma once

#include "Types.hpp"
#include <concepts>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <type_traits>

namespace resource {
template<class T>
concept SizedProvider = requires(const T& provider) {
	{ provider.size() } noexcept -> std::same_as<FileSize>;
};
template<class T>
concept ReadableProvider = requires(const T& provider, Cursor cursor, Output output) {
	{ provider.read(cursor, output) } noexcept -> std::same_as<ReadResult>;
};
template<class T>
concept WritableProvider = requires(T& provider, Cursor cursor, Input input, bool final) {
	{ provider.write(cursor, input, final) } noexcept -> std::same_as<WriteResult>;
};
// Persists what the file holds now (what write() applied): save(cursor) from
// 0 until complete, a bounded step per request, so complete is said only
// once the bytes are really kept; a file that keeps them at once answers
// {Ok, cursor, true}.
template<class T>
concept SavableProvider = requires(T& provider, Cursor cursor) {
	{ provider.save(cursor) } noexcept -> std::same_as<SaveResult>;
};
template<class T>
concept Provider =
    !std::is_volatile_v<T> && SizedProvider<T> && (ReadableProvider<T> || WritableProvider<T>);

// Callbacks borrow the descriptor context for one synchronous operation.
// A missing read/write/save callback is the sole capability declaration.
struct FileOps {
	FileSize (*size)(const void*) noexcept;
	ReadResult (*read)(const void*, Cursor, Output) noexcept;
	WriteResult (*write)(void*, Cursor, Input, bool) noexcept;
	SaveResult (*save)(void*, Cursor) noexcept;
};

namespace detail {
[[noreturn]] inline void invalidDefinition() noexcept
{
	std::abort();
}

// Paths are flat labels. These spelling checks never build or traverse a tree.
constexpr bool validPath(std::string_view path) noexcept
{
	if (path.size() < 2 || path.front() != '/' || path.back() == '/') {
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

template<Provider T>
inline constexpr FileOps operations = [] {
	// One immutable operation table per provider type. Missing capabilities
	// remain null; no type inspection takes place during a runtime request.
	FileOps ops{};
	ops.size = +[](const void* context) noexcept {
		return static_cast<const T*>(context)->size();
	};
	if constexpr (ReadableProvider<T>) {
		ops.read = +[](const void* context, Cursor cursor, Output output) noexcept {
			return static_cast<const T*>(context)->read(cursor, output);
		};
	}
	if constexpr (WritableProvider<T>) {
		ops.write = +[](void* context, Cursor cursor, Input input, bool final) noexcept {
			return static_cast<T*>(context)->write(cursor, input, final);
		};
	}
	if constexpr (SavableProvider<T>) {
		ops.save = +[](void* context, Cursor cursor) noexcept {
			return static_cast<T*>(context)->save(cursor);
		};
	} else {
		// A save() that is not exactly this one would leave the file silently
		// not Savable, and what it writes never kept.
		static_assert(
		    !requires { &T::save; },
		    "save() must be SaveResult save(Cursor) noexcept, callable on the provider");
	}
	return ops;
}();
} // namespace detail

// Only file() constructs descriptors. Strings and providers must outlive the
// filesystem; copying this descriptor never copies its provider.
// Public methods:
// - FileEntry(copy/move): Copy borrowed descriptor.
// - operator=(): Disable descriptor reassignment.
class FileEntry {
public:
	constexpr FileEntry(const FileEntry&) noexcept = default;
	constexpr FileEntry(FileEntry&&) noexcept = default;
	FileEntry& operator=(const FileEntry&) = delete;
	FileEntry& operator=(FileEntry&&) = delete;

private:
	const std::string_view path;
	void* const object;
	const FileOps* const ops;

	friend class FileView;
	friend class FileSystemView;
	template<std::size_t>
	friend class FileSystem;

	template<Provider T>
	friend constexpr FileEntry file(std::string_view, T&) noexcept;

	constexpr FileEntry(std::string_view label, void* context, const FileOps* functions) noexcept
	    : path(label), object(context), ops(functions)
	{}
};

// Bind a live provider to a validated flat label; no provider method runs here.
// Invalid path spelling is a definition error, including at constant evaluation.
template<Provider T>
constexpr FileEntry file(std::string_view path, T& provider) noexcept
{
	if (!detail::validPath(path)) {
		detail::invalidDefinition();
	}
	// Constness is retained in T and restored by the thunk. A const provider
	// can only expose write if that operation itself is callable on const T.
	return {path, const_cast<void*>(static_cast<const void*>(std::addressof(provider))),
	        &detail::operations<T>};
}
// Deduce the actual argument independently of any explicitly supplied provider
// type, so a converting temporary cannot leave a dangling provider reference.
// Invalid lvalue providers remain subject to the Provider concept diagnostics.
template<class... Explicit, class Actual>
    requires(!std::is_lvalue_reference_v<Actual> ||
             !(std::is_convertible_v<std::add_pointer_t<std::remove_reference_t<Actual>>,
                                     std::add_pointer_t<Explicit>> &&
               ...))
FileEntry file(std::string_view, Actual&&) = delete;
// A braced argument has no deducible Actual. With an explicit provider type,
// still reject a temporary while permitting {provider} to bind a live lvalue.
template<Provider T>
    requires(!std::is_reference_v<T>)
FileEntry file(std::string_view, std::remove_reference_t<T>&&) = delete;
// Braces around a conversion wrapper must not hide its actual type either.
template<class... Explicit, class Actual>
    requires(!(std::is_convertible_v<Actual*, std::add_pointer_t<Explicit>> && ...))
FileEntry file(std::string_view, std::initializer_list<Actual>) = delete;
// Reject owning temporary strings without pulling <string> into this header.
// Keep T first, just as in the live-provider overload, so Path is still deduced
// when the caller supplies an explicit provider type.
template<class T, class Path>
    requires(!std::is_lvalue_reference_v<Path> &&
             !std::is_same_v<std::remove_cvref_t<Path>, std::string_view> &&
             !std::is_pointer_v<std::remove_cvref_t<Path>> &&
             !std::is_array_v<std::remove_reference_t<Path>>)
FileEntry file(Path&&, T&) = delete;
// Braces suppress reference deduction, so owning strings and conversion
// wrappers in a braced path are rejected even when the element is an lvalue.
template<class T, class Path>
    requires(!std::is_same_v<std::remove_cvref_t<Path>, std::string_view> &&
             !std::is_pointer_v<std::remove_cvref_t<Path>> &&
             !std::is_array_v<std::remove_reference_t<Path>>)
FileEntry file(std::initializer_list<Path>, T&) = delete;
} // namespace resource

#endif // TELEMETRY_LIB_RESOURCE_FILE_HPP

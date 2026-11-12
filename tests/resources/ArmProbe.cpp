/* Generic resource layout and constant-provider dispatch, without telemetry.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT. */
// Provides a fixed-provider resource protocol probe with asserted 32-bit ARM layouts.
// It exposes direct provider dispatch for offline compilation and assembly inspection, not a hardware measurement.

#include <resource/FileSystem.hpp>
#include <resource/protocol/Protocol.hpp>
#include <cstddef>

// Fixed one-byte provider keeps offline dispatch inspection independent of application state.
// Public methods:
// - size(): Report fixed extent.
// - read(): Serve fixed byte.
struct ProbeProvider {
	resource::FileSize size() const noexcept
	{
		return 1;
	}

	resource::ReadResult read(resource::Cursor cursor, resource::Output out) const noexcept
	{
		if (cursor > 1)
			return {resource::Status::InvalidCursor, cursor};
		if (cursor == 1)
			return {resource::Status::Ok, cursor, 0, true};
		if (out.empty())
			return {resource::Status::BufferTooSmall, cursor};
		out[0] = std::byte{42};
		return {resource::Status::Ok, 1, 1, true};
	}
} resource_probe_provider;

extern constexpr auto resource_probe_files =
    resource::filesystem(resource::file("/probe", resource_probe_provider));
static_assert(sizeof(resource::FileEntry) == 16 && alignof(resource::FileEntry) == 4);
// FileSystem<1> above also checks the private path/object/ops offsets (0/8/12)
// in its production ARM32 layout assertions, alongside this size/alignment gate.
static_assert(sizeof(resource::FileSystemView) == 8);
static_assert(sizeof(resource::ReadResult) == 16 && alignof(resource::ReadResult) == 8);
static_assert(offsetof(resource::ReadResult, next) == 0 &&
              offsetof(resource::ReadResult, written) == 8 &&
              offsetof(resource::ReadResult, status) == 12 &&
              offsetof(resource::ReadResult, eof) == 13);
static_assert(sizeof(resource::WriteResult) == 16 && alignof(resource::WriteResult) == 8);
static_assert(offsetof(resource::WriteResult, next) == 0 &&
              offsetof(resource::WriteResult, consumed) == 8 &&
              offsetof(resource::WriteResult, status) == 12 &&
              offsetof(resource::WriteResult, complete) == 13);
static_assert(sizeof(resource::SaveResult) == 16 && alignof(resource::SaveResult) == 8);
static_assert(offsetof(resource::SaveResult, next) == 0 &&
              offsetof(resource::SaveResult, status) == 8 &&
              offsetof(resource::SaveResult, complete) == 9);
static_assert(sizeof(resource::FileStat) == 8 && alignof(resource::FileStat) == 4);
static_assert(offsetof(resource::FileStat, size) == 0 &&
              offsetof(resource::FileStat, status) == 4 &&
              offsetof(resource::FileStat, flags) == 5);

extern "C" resource::ReadResult resource_probe_read(resource::FileSystemView view,
                                                    resource::FileIndex index,
                                                    resource::Cursor cursor, std::byte* out,
                                                    std::size_t size) noexcept
{
	return view.read(index, cursor, {out, size});
}

extern "C" resource::ReadResult resource_probe_known(std::byte* out, std::size_t size) noexcept
{
	return resource_probe_files.read(0, 0, {out, size});
}

extern "C" resource::FileStat resource_probe_stat(resource::FileSystemView view,
                                                  resource::FileIndex index) noexcept
{
	return view.stat(index);
}

int main()
{
	const std::byte request[9]{std::byte{1}};
	std::byte reply[64];
	return resource::protocol::process(resource_probe_files.view(), request, reply).status ==
	               resource::Status::Ok
	           ? 0
	           : 1;
}

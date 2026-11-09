/**
 * @file DeviceResources.cpp
 * @brief Runtime facade for the demo's v3 descriptor and live values.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 *
 * Build descriptor bytes and a live values provider for facade tests.
 * Static owners and model-sized scratch give the generic facade stable
 * bindings without a separate initialization phase.
 */

#include "DeviceResources.hpp"
#include "../demo/DemoCatalog.h"
#include <resource/Resource.hpp>
#include <resource/protocol/Protocol.hpp>
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>

namespace {
namespace v3 = resource::telemetry::v3;

// Immutable metadata is constructed at compile time. The value provider
// borrows caller-owned scratch and does not require a later init() phase.
inline constexpr ::telemetry::descriptor::Descriptor descriptor{demo::model};
static_assert(descriptor.valid(), "The resource descriptor must describe a valid model");
inline constexpr auto descriptorBytes = ::telemetry::descriptor::packDescriptor<descriptor>();
inline constexpr v3::DescriptorFile descriptorFile{descriptorBytes};

// Grow with the model instead of guessing a scratch capacity. This bound also
// permits application writes to share the workspace if they are added later.
alignas(32) std::array<std::byte, demo::model.maxFieldScratch()> scratch;
telemetry::Workspace workspace{scratch};
inline constexpr v3::ValuesFile values{descriptor, workspace};
static_assert(values.requiredWorkspace() <= scratch.size());
constinit const auto fs =
    resource::filesystem(resource::file("/telemetry/descriptor.bin", descriptorFile),
                         resource::file("/telemetry/values.bin", values));
static_assert(decltype(fs)::fileCount() == 2);
} // namespace

namespace device::resources {
resource::FileSystemView files() noexcept
{
	return fs.view();
}

std::size_t fileCount() noexcept
{
	return fs.fileCount();
}

std::string_view path(resource::FileIndex index) noexcept
{
	return fs.path(index);
}

resource::FileStat stat(resource::FileIndex index) noexcept
{
	return fs.stat(index);
}

resource::ReadResult read(resource::FileIndex index, resource::Cursor cursor,
                          resource::Output output) noexcept
{
	return fs.read(index, cursor, output);
}

resource::WriteResult write(resource::FileIndex index, resource::Cursor cursor,
                            resource::Input input, bool final) noexcept
{
	return fs.write(index, cursor, input, final);
}

std::size_t handle(resource::Input request, resource::Output response) noexcept
{
	return resource::protocol::process(fs.view(), request, response).written;
}
} // namespace device::resources

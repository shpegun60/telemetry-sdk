// Cortex-M7 controls for the runtime ID boundary. Wide inputs must check their
// high word; the ordinary uint32_t dispatch remains the existing direct path.
// Authors: Ruslan Kovtun (shpegun60), codexAi. License: MIT.
// Provides small assembly inspection wrappers for wide identifier validation and direct local access.
// The wrappers separate high-bit rejection from the known-target path without claiming target timing.

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>

using namespace telemetry;
float idBoundaryRead() noexcept;
CommandResult idBoundaryCommand() noexcept;

namespace {
constexpr FieldTable rows{field<&idBoundaryRead>("value")};
constexpr testing::FieldCatalogTable fields{testing::group("fields", rows)};
constexpr CommandTable commands{command<&idBoundaryCommand>("run")};
constexpr testing::CommandCatalogTable grouped{testing::group("commands", commands)};
} // namespace

extern "C" {
CommandResult id_boundary_local_u32(std::uint32_t id) noexcept
{
	CommandResult result = CommandResult::NotFound;
	(void)commands.visit(id, [&](const auto& endpoint) noexcept {
		result = endpoint.call();
	});
	return result;
}

CommandResult id_boundary_local_u64(std::uint64_t id) noexcept
{
	CommandResult result = CommandResult::NotFound;
	(void)commands.visit(id, [&](const auto& endpoint) noexcept {
		result = endpoint.call();
	});
	return result;
}

const FieldEntry* id_boundary_field_u32(std::uint32_t id) noexcept
{
	return fields.index().find(id);
}

const FieldEntry* id_boundary_field_u64(std::uint64_t id) noexcept
{
	return fields.index().find(id);
}

const CommandEntry* id_boundary_command_u32(std::uint32_t id) noexcept
{
	return grouped.index().find(id);
}

const CommandEntry* id_boundary_command_u64(std::uint64_t id) noexcept
{
	return grouped.index().find(id);
}

GroupId id_boundary_group_u32(std::uint32_t id) noexcept
{
	return groupOf(id);
}

GroupId id_boundary_group_u64(std::uint64_t id) noexcept
{
	return groupOf(id);
}

EntryOffset id_boundary_index_u32(std::uint32_t id) noexcept
{
	return indexOf(id);
}

EntryOffset id_boundary_index_u64(std::uint64_t id) noexcept
{
	return indexOf(id);
}
}

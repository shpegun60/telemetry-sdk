/*
 * @file DecoderSize.cpp
 * @brief Cortex-M7 root of RuntimeSchema::parse for the decoder code-size report.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * run.py links this root alone with --gc-sections against the current tree
 * and against the base revision's lib/telemetry, and reports .text/.rodata
 * of each: the cost of reading v3.1 names and skipping attribute areas on a
 * client MCU. It is an offline object measurement, not a device execution.
 */

#include <telemetry/runtime/RuntimeSchema.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
namespace rt = telemetry::runtime;
std::array<rt::RuntimeType, 32> types;
std::array<rt::RuntimeMember, 32> members;
std::array<rt::RuntimeEnumEntry, 32> enums;
std::array<rt::RuntimeCatalog, 8> catalogs;
std::array<rt::RuntimeEndpoint, 32> endpoints;
} // namespace

extern "C" __attribute__((used)) std::uint32_t decoder_root(const std::byte* data,
                                                            std::uint32_t size) noexcept
{
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	const auto result = rt::RuntimeSchema::parse(std::span<const std::byte>{data, size}, workspace);
	return static_cast<std::uint32_t>(result.status) |
	       static_cast<std::uint32_t>(result.error) << 8 |
	       static_cast<std::uint32_t>(result.requirements.types) << 16;
}

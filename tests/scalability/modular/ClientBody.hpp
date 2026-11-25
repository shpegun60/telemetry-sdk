/*
 * One consumer body instantiated in separately compiled client translation units.
 *
 * Both modes perform the same encoded Field, Command and Service actions.
 * Typed consumers additionally see the full composition and instantiate native
 * calls with compile-time IDs; runtime consumers include only the public boundary.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
#ifndef TELEMETRY_TESTS_SCALABILITY_MODULAR_CLIENT_BODY_HPP
#define TELEMETRY_TESTS_SCALABILITY_MODULAR_CLIENT_BODY_HPP
#pragma once

#if SCALABILITY_TYPED_HEADER
#include "Composition.hpp"
#endif
#include "Runtime.hpp"
#include <telemetry/model/Adapter.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace {
constexpr auto selectedId = std::uint32_t{SCALABILITY_CLIENT_INDEX % 3} << 16;
constexpr auto submitted = std::uint32_t{100 + SCALABILITY_CLIENT_INDEX};

// Wire helpers deliberately know scalar byte layout, not private module DTOs.
void put32(std::span<std::byte> output, std::uint32_t value) noexcept
{
	for (unsigned i = 0; i < 4; ++i) {
		output[i] = std::byte((value >> (i * 8)) & 0xffu);
	}
}

std::uint32_t get32(std::span<const std::byte> input) noexcept
{
	std::uint32_t result = 0;
	for (unsigned i = 0; i < 4; ++i) {
		result |= std::uint32_t(std::to_integer<unsigned char>(input[i])) << (i * 8);
	}
	return result;
}
} // namespace

// Each generated function is linked once and called by the common driver.
extern "C" bool SCALABILITY_CLIENT_FUNCTION() noexcept
{
#if SCALABILITY_TYPED_HEADER
	const auto before =
	    telemetry::testing::CatalogAccess::read<selectedId>(modular::composition::fields);
	if (!before) {
		return false;
	}
	using Reading = std::remove_cvref_t<decltype(*before)>;
	if (telemetry::testing::CatalogAccess::write<selectedId>(
	        modular::composition::fields, Reading{submitted}) != telemetry::WriteResult::Applied) {
		return false;
	}
	if (telemetry::testing::CatalogAccess::call<selectedId>(modular::composition::commands,
	                                                        modular::Delta{2}) !=
	    telemetry::CommandResult::Executed) {
		return false;
	}
	const auto inspected = telemetry::testing::CatalogAccess::call<selectedId>(
	    modular::composition::services, modular::Query{3});
	if (!inspected.hasValue() || inspected.value().value != (submitted + 2) * 3) {
		return false;
	}
#endif
	// Also exercise the compiled encoded boundary in both modes.
	telemetry::Workspace workspace{std::span<std::byte>{}};
	const auto& view = modular::modelView();
	std::array<std::byte, 4> input{}, output{};
	put32(input, submitted);
	const auto written = telemetry::writeFieldEncoded(view, selectedId, input, workspace);
	if (written.dispatch != telemetry::DispatchStatus::Ok ||
	    written.endpointStatus != telemetry::WriteResult::Applied) {
		return false;
	}
	put32(input, 2);
	const auto advanced = telemetry::executeCommandEncoded(view, selectedId, input, workspace);
	if (advanced.dispatch != telemetry::DispatchStatus::Ok ||
	    advanced.endpointStatus != telemetry::CommandResult::Executed) {
		return false;
	}
	const auto read = telemetry::readFieldEncoded(view, selectedId, output, workspace);
	if (read.dispatch != telemetry::DispatchStatus::Ok || read.written != output.size() ||
	    get32(output) != submitted + 2) {
		return false;
	}
	put32(input, 3);
	const auto inspectedWire =
	    telemetry::callServiceEncoded(view, selectedId, input, output, workspace);
	return inspectedWire.dispatch == telemetry::DispatchStatus::Ok &&
	       inspectedWire.endpointStatus == telemetry::ServiceStatus::Ok &&
	       inspectedWire.written == output.size() && get32(output) == (submitted + 2) * 3 &&
	       workspace.used() == 0;
}
#endif

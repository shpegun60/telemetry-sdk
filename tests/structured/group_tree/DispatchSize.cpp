/*
 * @file DispatchSize.cpp
 * @brief ARM size of encoded dispatch over the same endpoints, group tree against flat catalogs.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * run.py links dispatch_root alone with --gc-sections, built with
 * GROUP_TREE=1 (Model{adc, metrology}) and GROUP_TREE=0 (the same endpoints in
 * today's three flat catalogs: four Field groups, one Command and one Service
 * group), and compares .text, .rodata and .data. Dispatch is the same O(1)
 * code; a tree adds one empty row per group without a family. With
 * DESCRIPTOR_SECTION the tree's descriptor bytes go to .descriptor.tree, so an
 * ARM build can be compared with tree.hex. These are offline object sizes.
 */

#include "AdcBranch.hpp"
#include "MetrologyBranch.hpp"

#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/testing/V30Model.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace dispatch_size {
namespace ts = telemetry;

#if GROUP_TREE
inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};
#else
using adc::Adc;
using adc::tlm::instance;
inline constexpr ts::FieldTable adcFields{
    ts::field<&Adc::config, &Adc::setConfig>("Config", instance),
    ts::field<&Adc::status>("Status", instance)};
inline constexpr ts::FieldTable channel1{
    ts::field<&Adc::gain<0>, &Adc::setGain<0>>("Gain", instance)};
inline constexpr ts::FieldTable channel2{
    ts::field<&Adc::gain<1>, &Adc::setGain<1>>("Gain", instance)};
inline constexpr ts::FieldTable meter{
    ts::field<&metrology::Meter::config, &metrology::Meter::setConfig>("Config",
                                                                       metrology::tlm::instance)};
inline constexpr ts::CommandTable adcCommands{ts::command<&Adc::reset>("Reset", instance)};
inline constexpr ts::ServiceTable adcServices{ts::service<&Adc::calibrate>("Calibrate", instance)};
inline constexpr ts::testing::FieldCatalogTable fields{
    ts::testing::group("adc", adcFields), ts::testing::group("channel1", channel1),
    ts::testing::group("channel2", channel2), ts::testing::group("metrology", meter)};
inline constexpr ts::testing::CommandCatalogTable commands{ts::testing::group("adc", adcCommands)};
inline constexpr ts::testing::ServiceCatalogTable services{ts::testing::group("adc", adcServices)};
// The same endpoints as per-family catalogs, the engine under the tree.
inline constexpr ts::testing::V30Model model{fields, commands, services};
#endif

#if GROUP_TREE && DESCRIPTOR_SECTION
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
#endif

// A provider keeps one ModelView; dispatch reads only its rows.
inline constexpr ts::ModelView view = model.view();

} // namespace dispatch_size

#if GROUP_TREE && DESCRIPTOR_SECTION
extern "C" {
__attribute__((used, section(".descriptor.tree"))) const auto group_tree_descriptor =
    ::telemetry::descriptor::packDescriptor<dispatch_size::descriptor>();
}
#endif

// Every family's encoded entry point, selected at run time.
extern "C" __attribute__((used)) std::uint32_t dispatch_root(std::uint32_t family, std::uint32_t id,
                                                             std::byte* data, std::uint32_t size,
                                                             std::byte* scratch,
                                                             std::uint32_t scratchSize) noexcept
{
	constexpr auto& view = dispatch_size::view;
	telemetry::Workspace workspace{std::span<std::byte>{scratch, scratchSize}};
	const std::span<std::byte> bytes{data, size};
	switch (family) {
		case 0:
			return view.fields.readEncoded(id, bytes, workspace).written;
		case 1:
			return static_cast<std::uint32_t>(
			    view.fields.writeEncoded(id, bytes, workspace).dispatch);
		case 2:
			return static_cast<std::uint32_t>(
			    view.commands.executeEncoded(id, bytes, workspace).dispatch);
		default:
			return view.services.callEncoded(id, bytes, bytes, workspace).written;
	}
}

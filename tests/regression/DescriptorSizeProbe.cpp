/*
 * @file DescriptorSizeProbe.cpp
 * @brief Cortex-M7 size of serving a descriptor: names or none, packed or streamed, tree or flat.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * run_arm_checks.py links descriptor_probe alone with --gc-sections for every
 * combination of three switches and reports .text, .rodata and .data:
 *   MODEL 0  the NUCLEO-H7S3L8 demo's flat Model (four named types)
 *   MODEL 1  the group-tree fixture as a tree, Model{adc, metrology}
 *   MODEL 2  the same endpoints as today's flat catalogs
 *   FORM 0   the packed descriptor: consteval bytes in .rodata, copied out
 *   FORM 1   the streaming descriptor: bytes made by DescriptorView::read()
 *   TELEMETRY_TYPE_NAMES 1 or 0 on the command line, the project switch.
 * A packed descriptor with names is larger than without by exactly the name
 * records, checked here at compile time. These are offline linked sizes.
 */

#if MODEL == 0
#include "../../examples/nucleo_h7s3l8/Protocol.hpp"
#else
#include "../structured/group_tree/AdcBranch.hpp"
#include "../structured/group_tree/MetrologyBranch.hpp"
#endif

#include <telemetry/descriptor/Descriptor.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace size_probe {
namespace ts = telemetry;

#if MODEL == 0
inline constexpr auto& model = nucleo_demo::model;
#elif MODEL == 1
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
inline constexpr ts::FieldCatalogTable fields{
    ts::group("adc", adcFields), ts::group("channel1", channel1), ts::group("channel2", channel2),
    ts::group("metrology", meter)};
inline constexpr ts::CommandCatalogTable commands{ts::group("adc", adcCommands)};
inline constexpr ts::ServiceCatalogTable services{ts::group("adc", adcServices)};
inline constexpr ts::Model model{fields, commands, services};
#endif

inline constexpr ts::descriptor::Descriptor descriptor{model};

#if TELEMETRY_TYPE_NAMES
// The names cost exactly their records: a u32 length and the bytes, per name.
using Registry = typename std::remove_cvref_t<decltype(model)>::Registry;
inline constexpr auto stripped = ts::descriptor::withoutTypeNames(model);

consteval std::size_t nameRecordBytes() noexcept
{
	std::size_t bytes = 0;
	for (const auto name : Registry::typeNames())
		if (!name.empty())
			bytes += 4 + name.size();
	return bytes;
}

static_assert(nameRecordBytes() > 0);
static_assert(descriptor.size() - stripped.size() == nameRecordBytes());
#endif

} // namespace size_probe

#if FORM == 0
extern "C" {
__attribute__((used)) const auto descriptor_packed =
    ::telemetry::descriptor::packDescriptor<size_probe::descriptor>();
}
#endif

// Copy up to size bytes of the descriptor from cursor; the number copied.
extern "C" __attribute__((used)) std::uint32_t
descriptor_probe(std::uint32_t cursor, std::byte* out, std::uint32_t size) noexcept
{
#if FORM == 0
	std::uint32_t written = 0;
	while (written < size && cursor + written < descriptor_packed.size()) {
		out[written] = descriptor_packed[cursor + written];
		++written;
	}
	return written;
#else
	return size_probe::descriptor.read(cursor, std::span<std::byte>{out, size}).written;
#endif
}

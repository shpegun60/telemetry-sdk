/*
 * @file MetrologyTelemetry.hpp
 * @brief The metrology branch, with the one type name this example writes by hand.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Its Config is metrology::Config automatically, apart from adc::Config. A
 * template specialization such as Spectrum<4> has no automatic name, so this
 * file gives it one. A type's TypeReflection lives in the file of the branch
 * that owns the type, before its group expression, so that every translation
 * unit sees it before the first descriptor that contains the type.
 */

#ifndef TELEMETRY_EXAMPLES_GROUP_TREE_METROLOGY_TELEMETRY_HPP
#define TELEMETRY_EXAMPLES_GROUP_TREE_METROLOGY_TELEMETRY_HPP
#pragma once

#include "Meter.hpp"

#include <telemetry/Telemetry.hpp>

// An explicit name replaces the automatic one; noTypeName() would remove it.
template<>
struct telemetry::reflection::TypeReflection<metrology::Spectrum<4>> {
	static constexpr auto name = telemetry::reflection::typeName("metrology::Spectrum4");
};

namespace metrology::tlm {
namespace ts = telemetry;

inline Meter instance;

inline Config config() noexcept
{
	return instance.config();
}

inline ts::WriteResult setConfig(const Config& value) noexcept
{
	return instance.setConfig(value) ? ts::WriteResult::Applied : ts::WriteResult::InvalidValue;
}

inline Spectrum<4> spectrum() noexcept
{
	return instance.spectrum();
}

inline constexpr auto branch = ts::group("metrology", ts::field<&config, &setConfig>("Config"),
                                         ts::field<&spectrum>("Spectrum"));

} // namespace metrology::tlm

#endif

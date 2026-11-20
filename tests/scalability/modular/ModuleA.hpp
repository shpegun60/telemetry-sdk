/*
 * Private schema and table declarations for module A.
 *
 * Composition needs these exact DTO/binding types; runtime consumers do not.
 * Definitions and mutable business state live in ModuleA.cpp.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
#ifndef TELEMETRY_TESTS_SCALABILITY_MODULAR_MODULE_A_HPP
#define TELEMETRY_TESTS_SCALABILITY_MODULAR_MODULE_A_HPP
#pragma once

#include "Common.hpp"
#include <telemetry/field/FieldTable.hpp>
#include <telemetry/command/CommandTable.hpp>
#include <telemetry/service/ServiceTable.hpp>
#include <resource/BytesFile.hpp>
#include <cstdint>

namespace modular::moduleA {
// Exact C++ identity stays distinct from the same shape in other modules.
struct Reading {
	std::uint32_t value;
};

// Named thunks expose signatures to composition, not business implementation.
Reading read() noexcept;
telemetry::WriteResult write(const Reading& value) noexcept;
telemetry::CommandResult advance(const Delta& request) noexcept;
Reading inspect(const Query& request) noexcept;

using Fields = telemetry::FieldTable<decltype(telemetry::field<&read, &write>("Value"))>;
using Commands = telemetry::CommandTable<decltype(telemetry::command<&advance>("Advance"))>;
using Services = telemetry::ServiceTable<decltype(telemetry::service<&inspect>("Inspect"))>;

// These nonmoving tables own definitions; their process-lifetime addresses are stable.
extern const Fields fields;
extern const Commands commands;
extern const Services services;
extern const resource::BytesFile version;

} // namespace modular::moduleA
#endif

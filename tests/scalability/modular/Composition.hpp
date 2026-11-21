/*
 * One typed composition point for all private module schemas.
 *
 * The typed-header build includes this in every client; the erased-boundary
 * build includes it only in Composition.cpp. One Registry deduplicates shared
 * requests while retaining each module's exact Reading type.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
#ifndef TELEMETRY_TESTS_SCALABILITY_MODULAR_COMPOSITION_HPP
#define TELEMETRY_TESTS_SCALABILITY_MODULAR_COMPOSITION_HPP
#pragma once

#include "ModuleA.hpp"
#include "ModuleB.hpp"
#include "ModuleC.hpp"
#include <telemetry/model/Model.hpp>
#include <telemetry/testing/V30Model.hpp>

namespace modular::composition {
inline constinit const telemetry::testing::FieldCatalogTable fields{
    telemetry::testing::group("A", moduleA::fields),
    telemetry::testing::group("B", moduleB::fields),
    telemetry::testing::group("C", moduleC::fields)};
inline constinit const telemetry::testing::CommandCatalogTable commands{
    telemetry::testing::group("A", moduleA::commands),
    telemetry::testing::group("B", moduleB::commands),
    telemetry::testing::group("C", moduleC::commands)};
inline constinit const telemetry::testing::ServiceCatalogTable services{
    telemetry::testing::group("A", moduleA::services),
    telemetry::testing::group("B", moduleB::services),
    telemetry::testing::group("C", moduleC::services)};
inline constexpr telemetry::testing::V30Model model{fields, commands, services};
using Registry = decltype(model)::Registry;
static_assert(Registry::typeCount == 17);
static_assert(Registry::typeId<moduleA::Reading>() != Registry::typeId<moduleB::Reading>());
static_assert(Registry::typeId<moduleB::Reading>() != Registry::typeId<moduleC::Reading>());
} // namespace modular::composition
#endif

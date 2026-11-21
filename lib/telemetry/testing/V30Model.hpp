/*
 * @file V30Model.hpp
 * @brief A per-family catalog Model for checks only: the producer of descriptor v3.0 bytes.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Not a way to write a device: a device is a group tree, Model{ts::group(...)}.
 *
 * A tree always carries the GroupTree flag, so its descriptor is v3.1 even
 * without type names. The frozen v3.0 goldens, the clients' v3.0 reading
 * checks and an emulator's legacy profile still need v3.0 bytes: per-family
 * catalogs, where a Field group and a Command group may share a name and a
 * Field and a Command may share a path. This header names that producer, the
 * flat engine under every tree, so a check needs nothing else of the
 * per-family form:
 *
 *   namespace v30 = telemetry::testing;
 *   inline constexpr v30::FieldCatalogTable fields{v30::group("motor", motorFields)};
 *   inline constexpr v30::V30Model model{fields, v30::emptyCommands, services};
 *   inline constexpr auto descriptor = telemetry::descriptor::withoutTypeNames(model);
 *
 * withoutTypeNames (or TELEMETRY_TYPE_NAMES=0) gives v3.0 bytes; with names
 * a V30Model gives the flat v3.1 form that the named goldens freeze.
 * Telemetry.hpp does not include this header, the SDK does not install it,
 * and the user documentation does not describe it.
 */

#ifndef TELEMETRY_TESTING_V30_MODEL_HPP
#define TELEMETRY_TESTING_V30_MODEL_HPP
#pragma once

#include "../model/Model.hpp"

namespace telemetry::testing {

// The per-family catalog tables, the group(name, table) row that labels one
// local table, and the explicit empty Field and Command catalogs.
using ::telemetry::CommandCatalogTable;
using ::telemetry::EmptyEndpointCatalog;
using ::telemetry::emptyCommands;
using ::telemetry::emptyFields;
using ::telemetry::FieldCatalogTable;
using ::telemetry::group;
using ::telemetry::ServiceCatalogTable;

// A Model over three per-family catalog tables. It borrows them, as the
// engine does: they and their local tables must outlive it. Descriptor,
// withoutTypeNames, ModelView and every bound take it as they take the engine.
template<class Fields, class Commands, class Services>
class V30Model : public Model<Fields, Commands, Services> {
public:
	using Model<Fields, Commands, Services>::Model;
};

template<class Fields, class Commands, class Services>
V30Model(const Fields&, const Commands&, const Services&) -> V30Model<Fields, Commands, Services>;

} // namespace telemetry::testing

#endif

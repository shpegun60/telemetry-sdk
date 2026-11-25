/*
 * @file Removed.cpp
 * @brief The removed public forms no longer compile from Telemetry.hpp alone.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * One way: the group tree, paths and handles. The flat Model, its catalog
 * tables and empty catalogs, the catalog form of group(name, table), the
 * public makeId and typed access by a compile-time ID are gone from the
 * public API, and the testing-only v3.0 producer is not reachable from
 * Telemetry.hpp. run.py compiles one CASE at a time with -fsyntax-only:
 * cases below 100 must fail with their own diagnostic, cases from 100 are
 * controls that must build. Nothing here includes telemetry/testing.
 */

#include <telemetry/Telemetry.hpp>

#include <cstdint>

namespace removed {
namespace ts = telemetry;

inline float gain = 1.0f;

inline float readGain() noexcept
{
	return gain;
}

inline telemetry::WriteResult setGain(const float& value) noexcept
{
	gain = value;
	return telemetry::WriteResult::Applied;
}

inline telemetry::CommandResult reset() noexcept
{
	return telemetry::CommandResult::Executed;
}

struct Count {
	std::uint32_t value;
};

inline Count twice(const Count& count) noexcept
{
	return {count.value * 2u};
}

inline constexpr auto branch =
    ts::group("adc", ts::field<&readGain, &setGain>("Gain"), ts::command<&reset>("Reset"),
              ts::service<&twice>("Twice"));
inline constexpr ts::Model model{branch};

// The local tables a device can still write, for the cases that try the
// removed catalog forms over them.
inline constexpr ts::FieldTable fieldRows{ts::field<&readGain, &setGain>("Gain")};
inline constexpr ts::CommandTable commandRows{ts::command<&reset>("Reset")};
inline constexpr ts::ServiceTable serviceRows{ts::service<&twice>("Twice")};

#if CASE == 1
// The flat three-catalog Model has no deduction guide.
inline constexpr ts::Model flat{fieldRows, commandRows, serviceRows};
#elif CASE == 2
inline constexpr ts::FieldCatalogTable catalogs{};
#elif CASE == 3
inline constexpr ts::CommandCatalogTable catalogs{};
#elif CASE == 4
inline constexpr ts::ServiceCatalogTable catalogs{};
#elif CASE == 5
// group(name, table) labels no local table any more: groups hold endpoints.
inline constexpr auto row = ts::group("adc", fieldRows);
#elif CASE == 6
inline constexpr auto& none = ts::emptyCommands;
#elif CASE == 7
inline constexpr auto& none = ts::emptyFields;
#elif CASE == 8
inline constexpr ts::EmptyEndpointCatalog none{};
#elif CASE == 9
inline constexpr ts::TableGroup<decltype(fieldRows)> row{"adc", &fieldRows};
#elif CASE == 10
inline constexpr auto id = ts::makeId(0, 0);
#elif CASE == 11
inline constexpr auto id = ts::makeId<0, 0>();
#elif CASE == 12
inline constexpr auto id = ts::tryMakeId(0, 0);
#elif CASE == 13
// Typed access by a compile-time ID: a path or a handle names the endpoint.
inline const auto value = model.fields().read<ts::idOf<model, "adc/Gain">()>();
#elif CASE == 14
inline const auto status = model.fields().write<ts::idOf<model, "adc/Gain">()>(2.0f);
#elif CASE == 15
inline const auto& definition = model.fields().get<ts::idOf<model, "adc/Gain">()>();
#elif CASE == 16
inline const auto result = model.commands().call<ts::idOf<model, "adc/Reset">()>();
#elif CASE == 17
inline const auto result = model.services().call<ts::idOf<model, "adc/Twice">()>(Count{3u});
#elif CASE == 18
inline const auto value = model.fields().readAs<float, ts::idOf<model, "adc/Gain">()>();
#elif CASE == 19
// The engine under the tree is not a public member.
inline const auto& engine = model.flat();
#elif CASE == 20
// The testing-only v3.0 producer is not reachable from Telemetry.hpp.
using Producer = ts::testing::V30Model<int, int, int>;
#elif CASE == 100
// Control: the one way builds - paths, handles, idOf and handle.id, and the
// runtime-ID access a transport needs.
inline constexpr auto gainHandle = ts::endpoint<model, "adc/Gain">;
inline constexpr auto resetHandle = ts::endpoint<model, "adc/Reset">;
static_assert(gainHandle.id == ts::idOf<model, "adc/Gain">());
static_assert(model.idOf<"adc/Twice">() == ts::idOf<model, "adc/Twice">());
static_assert(ts::groupOf(resetHandle.id) == 0 && ts::indexOf(resetHandle.id) == 0);
inline const auto value = gainHandle.read();
inline const auto written = ts::write<model, "adc/Gain">(2.0f);
inline const auto called = resetHandle.call();
inline const auto doubled = ts::call<model, "adc/Twice">(Count{3u});
inline const auto runtime = model.fields().readAs<float>(gainHandle.id);
inline const bool visited = model.commands().visit(resetHandle.id, [](const auto&) noexcept {
});
#ifdef TELEMETRY_TESTING_V30_MODEL_HPP
#error Telemetry.hpp includes the testing-only v3.0 producer
#endif
#else
#error Unknown CASE
#endif

} // namespace removed

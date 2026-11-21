/*
 * @file Fixture.hpp
 * @brief Frozen v3.0 models: mixed endpoints, shared types, UTF-8 and empty shapes.
 *
 * Catalog models of telemetry/testing/V30Model.hpp, the producer of the frozen
 * descriptor v3.0 goldens mixed.hex, edge.hex and empty.hex; tree.hex is the
 * group-tree suite's.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#ifndef TELEMETRY_TESTS_STRUCTURED_DESCRIPTOR_FIXTURE_HPP
#define TELEMETRY_TESTS_STRUCTURED_DESCRIPTOR_FIXTURE_HPP
#pragma once

#include "../endpoints/MixedFixture.hpp"
#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/testing/V30Model.hpp>

namespace descriptor_fixture {
namespace ts = telemetry;

// Empty reflected shape verifies zero-size type records and canonical field tokens.
struct Empty {};

// Nested graph combines shared DTOs, a fixed array and two zero-wire-size members.
struct Box {
	fixture::MotorConfig config;
	std::array<fixture::State, 2> phases;
	Empty empty;
	std::array<std::uint8_t, 0> zero;
};
enum class Signed : std::int16_t {
	Low = -1000,
	High = 2000
};
enum class Wide : std::uint64_t {
	High = 0xffffffffffffffffULL
};
} // namespace descriptor_fixture

// Explicit signed enum dictionary fixes names and code order for independent wire goldens.
template<>
struct telemetry::reflection::EnumReflection<descriptor_fixture::Signed> {
	inline static constexpr auto entries = telemetry::reflection::enumEntries(
	    telemetry::reflection::enumEntry(descriptor_fixture::Signed::High, u8"Haut"),
	    telemetry::reflection::enumEntry(descriptor_fixture::Signed::Low, u8"N\u00e9gatif"));
};

// Explicit U64 enum dictionary preserves the all-ones code in descriptor bytes.
template<>
struct telemetry::reflection::EnumReflection<descriptor_fixture::Wide> {
	inline static constexpr auto entries = telemetry::reflection::enumEntries(
	    telemetry::reflection::enumEntry(descriptor_fixture::Wide::High, u8"Max"));
};

namespace descriptor_fixture {
inline int calls = 0;

template<class T>
T read() noexcept
{
	++calls;
	return {};
}

inline void ping() noexcept
{
	++calls;
}

inline telemetry::FunctionSlot<std::uint32_t() noexcept> getter;
inline telemetry::FunctionSlot<telemetry::WriteResult(std::uint32_t) noexcept> setter;
inline constexpr ts::FieldTable extraFields{
    ts::field<&read<Empty>>("Empty"), ts::field<&read<Box>>("Box"),
    ts::field<&read<Signed>>("Signed"), ts::field<&read<Wide>>("Wide"),
    ts::field("Late", getter, setter)};
inline constexpr ts::FieldTable<> noFields{};
inline constexpr ts::ServiceTable extraServices{ts::service<&ping>("Ping")};
inline constexpr ts::testing::FieldCatalogTable fields{
    ts::testing::group("motor", fixture::mixedFields), ts::testing::group("empty", noFields),
    ts::testing::group("\xCE\xBC", extraFields)};
inline constexpr ts::testing::ServiceCatalogTable services{
    ts::testing::group("motor", fixture::localServices),
    ts::testing::group("extra", extraServices)};
inline constexpr ts::testing::V30Model model{fields, fixture::v30Commands, services};
// Frozen v3.0 goldens: the profile strips type names on every build, so these
// bytes never depend on TELEMETRY_TYPE_NAMES; programs that include this file
// may also build named (v3.1) descriptors of the same models.
inline constexpr auto mixed = ::telemetry::descriptor::withoutTypeNames(fixture::v30Model);
inline constexpr auto edge = ::telemetry::descriptor::withoutTypeNames(model);
inline constexpr ts::testing::ServiceCatalogTable<> noServices{};
inline constexpr ts::testing::V30Model emptyModel{ts::testing::emptyFields,
                                                  ts::testing::emptyCommands, noServices};
inline constexpr auto empty = ::telemetry::descriptor::withoutTypeNames(emptyModel);
inline constexpr auto mixedBytes = ::telemetry::descriptor::packDescriptor<mixed>();
inline constexpr auto edgeBytes = ::telemetry::descriptor::packDescriptor<edge>();
inline constexpr auto emptyBytes = ::telemetry::descriptor::packDescriptor<empty>();
static_assert(mixed.valid() && edge.valid() && empty.valid());
static_assert(decltype(model)::typeId<fixture::MotorConfig>() == 15);

// Explicit decltype preserves each constexpr catalog's const qualification.
// Classification must match the deduced Model without changing wire identity.
using ExactCvModel =
    ts::testing::V30Model<decltype(fields), decltype(fixture::v30Commands), decltype(services)>;
inline constexpr ExactCvModel exactCvModel{fields, fixture::v30Commands, services};
inline constexpr auto exactCvDescriptor = ::telemetry::descriptor::withoutTypeNames(exactCvModel);
static_assert(exactCvDescriptor.valid() && exactCvDescriptor.fingerprint() == edge.fingerprint());
static_assert(::telemetry::descriptor::packDescriptor<exactCvDescriptor>() == edgeBytes);
} // namespace descriptor_fixture

#endif // TELEMETRY_TESTS_STRUCTURED_DESCRIPTOR_FIXTURE_HPP

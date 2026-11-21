/*
 * @file Negative.cpp
 * @brief Compile-time refusals and controls for group trees, ceilings, paths and handles.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * run.py compiles one CASE at a time with -fsyntax-only. Cases below 100 must
 * fail with their own diagnostic; cases from 100 are controls that must build.
 */

#include "AdcBranch.hpp"
#include "MetrologyBranch.hpp"

#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/testing/V30Model.hpp>

#include <array>
#include <cstdint>

namespace negative {
namespace ts = telemetry;

template<class T>
T read() noexcept
{
	return {};
}

inline telemetry::CommandResult act() noexcept
{
	return telemetry::CommandResult::Executed;
}

struct Wide {
	std::uint8_t aMemberNameLongerThanSixtyFourBytesWhichTheTreeCeilingRefusesHere_;
};

struct Plain {
	std::uint8_t value;
};

template<class Profile, class F, class C, class S>
constexpr auto withProfile(const ts::Model<F, C, S>& source) noexcept
{
	return ::telemetry::descriptor::Descriptor<F, C, S, Profile>{source};
}

inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};

// 65 bytes: one over the producer's default maxNameBytes.
#define NAME65 "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklm"
// 60 bytes: five levels of it make a path of 304 bytes.
#define NAME60 "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefgh"

} // namespace negative

#if CASE == 10 || CASE == 103
template<>
struct telemetry::reflection::TypeReflection<negative::Plain> {
	static constexpr auto name = telemetry::reflection::typeName("scope::" NAME65);
};
#endif

namespace negative {

#if CASE == 1
// A group name is a path segment.
inline constexpr auto bad = ts::group("adc/x", ts::field<&read<float>>("Gain"));
#elif CASE == 2
// An endpoint name of a tree is a path segment.
inline constexpr auto bad = ts::group("adc", ts::field<&read<float>>("Gain.x"));
#elif CASE == 3
// A Field and a Command of one group share a name.
inline constexpr auto bad =
    ts::group("adc", ts::field<&read<float>>("Reset"), ts::command<&act>("Reset"));
#elif CASE == 4
// A subgroup and a Field of one group share a name.
inline constexpr auto bad = ts::group("adc", ts::group("Gain"), ts::field<&read<float>>("Gain"));
#elif CASE == 5
// Two root branches share a name.
inline constexpr auto again = ts::group("adc", ts::field<&read<float>>("Other"));
inline constexpr ts::Model bad{adc::tlm::branch, again};
#elif CASE == 6 || CASE == 101 || CASE == 102
// Nine levels (depths 0..8) are one over the default maxGroupDepth of 8;
// eight levels build.
inline constexpr auto leaf = ts::group("l7", ts::field<&read<float>>("x"));
#if CASE == 6 || CASE == 102
inline constexpr auto deep = ts::group("l8", leaf);
#else
inline constexpr auto deep = leaf;
#endif
inline constexpr auto chain = ts::group(
    "l0",
    ts::group(
        "l1",
        ts::group("l2", ts::group("l3", ts::group("l4", ts::group("l5", ts::group("l6", deep)))))));
inline constexpr ts::Model tree{chain};
#if CASE == 102
struct Deeper : ts::Limits {
	static constexpr std::uint32_t maxGroupDepth = 16;
};

inline constexpr auto descriptor = withProfile<Deeper>(tree);
#else
inline constexpr ::telemetry::descriptor::Descriptor descriptor{tree};
#endif
static_assert(descriptor.valid());
#elif CASE == 7 || CASE == 103
// A 65-byte group name; a profile with a wider maxNameBytes accepts it, and
// the 65-byte part of a type name with it.
inline constexpr auto branch = ts::group(NAME65, ts::field<&read<Plain>>("x"));
inline constexpr ts::Model tree{branch};
#if CASE == 103
struct Wider : ts::Limits {
	static constexpr std::uint32_t maxNameBytes = 128;
};

inline constexpr auto descriptor = withProfile<Wider>(tree);
#else
inline constexpr ::telemetry::descriptor::Descriptor descriptor{tree};
#endif
static_assert(descriptor.valid());
#elif CASE == 8
// Five 60-byte levels: a 304-byte path, over the default maxPathBytes of 256.
inline constexpr auto branch = ts::group(
    NAME60 "0",
    ts::group(NAME60 "1",
              ts::group(NAME60 "2", ts::group(NAME60 "3", ts::field<&read<float>>(NAME60)))));
inline constexpr ts::Model tree{branch};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{tree};
#elif CASE == 9
// A member name is a path segment of a tree: adc/Wide.member.
inline constexpr auto branch = ts::group("adc", ts::field<&read<Wide>>("Wide"));
inline constexpr ts::Model tree{branch};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{tree};
#elif CASE == 10
// One part of a type name over maxNameBytes, in a flat model too.
inline constexpr ts::FieldTable rows{ts::field<&read<Plain>>("x")};
inline constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("g", rows)};
inline constexpr ts::testing::ServiceCatalogTable<> noServices{};
inline constexpr ts::testing::V30Model flat{catalogs, ts::testing::emptyCommands, noServices};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{flat};
#elif CASE == 11
inline const auto value = ts::read<model, "adc/channel3/Gain">();
#elif CASE == 12
inline const auto value = ts::read<model, "adc/Nope">();
#elif CASE == 13
inline const auto value = ts::read<model, "adc/channel1">();
#elif CASE == 14
inline const auto value = ts::read<model, "adc/Reset">();
#elif CASE == 15
inline const auto value = ts::read<model, "adc/Config.nope">();
#elif CASE == 16
inline const auto value = ts::read<model, "adc/Config.gain.x">();
#elif CASE == 17
inline const auto value = ts::read<model, "adc/Status.raw[2]">();
#elif CASE == 18
inline const auto value = ts::write<model, "adc/Config.gain">(1.0f);
#elif CASE == 19
// A whole Field of another type.
inline const auto value = ts::write<model, "adc/channel1/Gain">(adc::Config{});
#elif CASE == 20
inline constexpr auto id = model.idOf<"adc/nope">();
#elif CASE == 21
inline const auto value = ts::call<model, "adc/Config">();
#elif CASE == 22
inline const auto value = ts::read<model, "adc//Config">();
// Named handles: the path is checked where the handle is declared.
#elif CASE == 23
inline constexpr auto handle = ts::endpoint<model, "adc/channel3/Gain">;
#elif CASE == 24
inline constexpr auto handle = ts::endpoint<model, "adc/Nope">;
#elif CASE == 25
inline constexpr auto handle = ts::endpoint<model, "adc/channel1">;
#elif CASE == 26
inline constexpr auto handle = ts::endpoint<model, "adc/Config.nope">;
#elif CASE == 27
inline constexpr auto handle = ts::endpoint<model, "adc/Status.raw[2]">;
#elif CASE == 28
inline constexpr auto handle = ts::endpoint<model, "adc/Reset.x">;
#elif CASE == 29
inline constexpr auto handle = ts::endpoint<model, "adc//Config">;
#elif CASE == 30
// A member handle reads only.
inline constexpr auto handle = ts::endpoint<model, "adc/Config.gain">;
inline const auto value = handle.write(1.0f);
#elif CASE == 31
inline constexpr auto handle = ts::endpoint<model, "adc/Reset">;
inline const auto value = handle.read();
#elif CASE == 32
inline constexpr auto handle = ts::endpoint<model, "adc/Calibrate">;
inline const auto value = handle.write(adc::Reference{});
#elif CASE == 33
inline constexpr auto handle = ts::endpoint<model, "adc/Config">;
inline const auto value = handle.call();
#elif CASE == 34
// A whole Field of another type.
inline constexpr auto handle = ts::endpoint<model, "adc/channel1/Gain">;
inline const auto value = handle.write(adc::Config{});
#elif CASE == 35
// Paths address the group-tree form only.
inline constexpr ts::FieldTable rows{ts::field<&read<float>>("x")};
inline constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("g", rows)};
inline constexpr ts::testing::ServiceCatalogTable<> noServices{};
inline constexpr ts::testing::V30Model flat{catalogs, ts::testing::emptyCommands, noServices};
inline constexpr auto handle = ts::endpoint<flat, "g/x">;
#elif CASE == 104
// The flat form keeps any UTF-8: path characters and names past 64 bytes.
inline constexpr ts::FieldTable rows{ts::field<&read<float>>("a.b[0]/" NAME65)};
inline constexpr ts::testing::FieldCatalogTable catalogs{
    ts::testing::group("a/b.c\\d" NAME65, rows)};
inline constexpr ts::testing::ServiceCatalogTable<> noServices{};
inline constexpr ts::testing::V30Model flat{catalogs, ts::testing::emptyCommands, noServices};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{flat};
static_assert(descriptor.valid());
#elif CASE == 105
// One endpoint name in two groups, and two branches with a Config each.
static_assert(ts::idOf<model, "adc/channel1/Gain">() != ts::idOf<model, "adc/channel2/Gain">());
static_assert(ts::idOf<model, "adc/Config">() != ts::idOf<model, "metrology/Config">());
#elif CASE == 106
// Handles in a branch file's own namespace, usable in constant expressions,
// and a read-only Field's handle compiles a write that returns ReadOnly.
inline constexpr auto status = ts::endpoint<model, "adc/Status">;
inline constexpr auto copy = status;
static_assert(copy.id == ts::detail::makeId(0, 1) && !copy.member);
inline const auto refused = copy.write(adc::Status{});
#else
#error Unknown CASE
#endif

} // namespace negative

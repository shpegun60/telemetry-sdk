/*
 * @file Negative.cpp
 * @brief Compile-time refusals and controls for descriptor v3.1 type names.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * run.py compiles one CASE at a time with -fsyntax-only. Cases below 100 must
 * fail with their own diagnostic; cases from 100 are controls that must build.
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>

#include <array>
#include <cstdint>

namespace ts = telemetry;
namespace tr = telemetry::reflection;

namespace negative {
template<class T>
T read() noexcept
{
	return {};
}

namespace left {
struct Status {
	std::uint8_t code;
};
enum class Mode : std::uint8_t {
	A
};
} // namespace left

namespace right {
struct Status {
	std::uint16_t code;
};
enum class Mode : std::uint8_t {
	B
};
} // namespace right
} // namespace negative

#if CASE == 3
// An explicit name may itself collide with an automatic one.
template<>
struct telemetry::reflection::TypeReflection<negative::right::Status> {
	static constexpr auto name = telemetry::reflection::typeName("Mode");
};
#elif CASE == 103
template<>
struct telemetry::reflection::TypeReflection<negative::right::Status> {
	static constexpr auto name = telemetry::reflection::typeName("WideStatus");
};
#elif CASE == 104
template<>
struct telemetry::reflection::TypeReflection<negative::right::Status> {
	static constexpr auto name = telemetry::reflection::noTypeName();
};
#elif CASE == 7
template<>
struct telemetry::reflection::TypeReflection<std::array<int, 2>> {
	static constexpr auto name = telemetry::reflection::typeName("Pair");
};
#elif CASE == 8
template<>
struct telemetry::reflection::TypeReflection<negative::left::Status> {
	static constexpr int name = 1;
};
#endif

namespace negative {
inline constexpr ts::ServiceCatalogTable<> noServices{};

#if CASE == 1 || CASE == 3 || CASE == 9 || CASE == 101 || CASE == 102 || CASE == 103 || CASE == 104
// Two structs with the automatic short name Status (case 3: left::Mode too).
inline constexpr ts::FieldTable rows{ts::field<&read<left::Status>>("Left"),
                                     ts::field<&read<right::Status>>("Right"),
                                     ts::field<&read<left::Mode>>("Mode")};
#elif CASE == 2
// Two scoped enums with the automatic short name Mode.
inline constexpr ts::FieldTable rows{ts::field<&read<left::Mode>>("Left"),
                                     ts::field<&read<right::Mode>>("Right")};
#endif

#if CASE == 1 || CASE == 2 || CASE == 3 || CASE >= 101
inline constexpr ts::FieldCatalogTable catalogs{ts::group("g", rows)};
inline constexpr ts::Model model{catalogs, ts::emptyCommands, noServices};
#endif

#if CASE == 1 || CASE == 2 || CASE == 3 || CASE == 101 || CASE == 103 || CASE == 104
// 101 is built with TELEMETRY_TYPE_NAMES=0; 103 and 104 settle the clash.
constexpr ::telemetry::descriptor::Descriptor descriptor{model};
static_assert(descriptor.valid());
#elif CASE == 102
// A stripping profile emits no name, so names cannot clash.
constexpr auto descriptor = ::telemetry::descriptor::withoutTypeNames(model);
static_assert(descriptor.valid() && descriptor.view().size() > 0);
#elif CASE == 4
constexpr auto bad = tr::typeName("Mo\xff"
                                  "de");
#elif CASE == 5
constexpr auto bad = tr::typeName("Mo\0de");
#elif CASE == 6
constexpr auto bad = tr::typeName("");
#elif CASE == 7
constexpr auto bad = tr::typeName<std::array<int, 2>>();
#elif CASE == 8
constexpr auto bad = tr::typeName<left::Status>();
#elif CASE == 9
// A profile may tighten the string ceiling below a type name's length.
struct Profile : ts::Limits {
	static constexpr unsigned maxStringBytes = 3;
};

inline constexpr ts::FieldTable shortRows{ts::field<&read<left::Status>>("a")};
inline constexpr ts::FieldCatalogTable catalogs{ts::group("g", shortRows)};
inline constexpr ts::Model model{catalogs, ts::emptyCommands, noServices};

template<class P, class F, class C, class S>
constexpr auto withProfile(const ts::Model<F, C, S>& source) noexcept
{
	return ::telemetry::descriptor::Descriptor<F, C, S, P>{source};
}

constexpr auto bad = withProfile<Profile>(model);
#else
#error Unknown CASE
#endif
} // namespace negative

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
#include <telemetry/testing/V30Model.hpp>
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

// The automatic name keeps only the innermost scope, so left::io::Status and
// right::io::Status are both "io::Status".
namespace left::io {
struct Status {
	std::uint8_t code;
};
enum class Mode : std::uint8_t {
	A
};
} // namespace left::io

namespace right::io {
struct Status {
	std::uint16_t code;
};
enum class Mode : std::uint8_t {
	B
};
} // namespace right::io

// A class declared in a function has no stable scope.
inline auto makeLocal() noexcept
{
	struct Local {
		std::uint8_t raw;
	};

	return Local{};
}

using Local = decltype(makeLocal());
} // namespace negative

#if CASE == 3
// An explicit name may itself collide with an automatic one.
template<>
struct telemetry::reflection::TypeReflection<negative::right::io::Status> {
	static constexpr auto name = telemetry::reflection::typeName("io::Mode");
};
#elif CASE == 103
template<>
struct telemetry::reflection::TypeReflection<negative::right::io::Status> {
	static constexpr auto name = telemetry::reflection::typeName("io::WideStatus");
};
#elif CASE == 104
template<>
struct telemetry::reflection::TypeReflection<negative::right::io::Status> {
	static constexpr auto name = telemetry::reflection::noTypeName();
};
#elif CASE == 105
// A local class builds once it carries an explicit name.
template<>
struct telemetry::reflection::TypeReflection<negative::Local> {
	static constexpr auto name = telemetry::reflection::typeName("negative::Local");
};
#elif CASE == 7
template<>
struct telemetry::reflection::TypeReflection<std::array<int, 2>> {
	static constexpr auto name = telemetry::reflection::typeName("Pair");
};
#elif CASE == 8
template<>
struct telemetry::reflection::TypeReflection<negative::left::io::Status> {
	static constexpr int name = 1;
};
#endif

namespace negative {
inline constexpr ts::testing::ServiceCatalogTable<> noServices{};

#if CASE == 1 || CASE == 3 || CASE == 9 || CASE == 101 || CASE == 102 || CASE == 103 || CASE == 104
// Two structs with the automatic name io::Status (case 3: io::Mode too).
inline constexpr ts::FieldTable rows{ts::field<&read<left::io::Status>>("Left"),
                                     ts::field<&read<right::io::Status>>("Right"),
                                     ts::field<&read<left::io::Mode>>("Mode")};
#elif CASE == 2
// Two scoped enums with the automatic name io::Mode.
inline constexpr ts::FieldTable rows{ts::field<&read<left::io::Mode>>("Left"),
                                     ts::field<&read<right::io::Mode>>("Right")};
#elif CASE == 12 || CASE == 105 || CASE == 106 || CASE == 107
// A local class: a build error under automatic names (12), accepted with an
// explicit name (105), with names off (106) or through a stripping profile (107).
inline constexpr ts::FieldTable rows{ts::field<&read<Local>>("Local")};
#endif

#if CASE == 1 || CASE == 2 || CASE == 3 || CASE == 12 || CASE >= 101
inline constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("g", rows)};
inline constexpr ts::testing::V30Model model{catalogs, ts::testing::emptyCommands, noServices};
#endif

#if CASE == 1 || CASE == 2 || CASE == 3 || CASE == 12 || CASE == 101 || CASE == 103 ||             \
    CASE == 104 || CASE == 105 || CASE == 106
// 101 is built with TELEMETRY_TYPE_NAMES=0; 103 and 104 settle the clash.
constexpr ::telemetry::descriptor::Descriptor descriptor{model};
static_assert(descriptor.valid());
#elif CASE == 102 || CASE == 107
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
constexpr auto bad = tr::typeName<left::io::Status>();
#elif CASE == 9
// A profile may tighten the string ceiling below a type name's length.
struct Profile : ts::Limits {
	static constexpr unsigned maxStringBytes = 3;
};

inline constexpr ts::FieldTable shortRows{ts::field<&read<left::io::Status>>("a")};
inline constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("g", shortRows)};
inline constexpr ts::testing::V30Model model{catalogs, ts::testing::emptyCommands, noServices};

template<class P, class F, class C, class S>
constexpr auto withProfile(const ts::testing::V30Model<F, C, S>& source) noexcept
{
	return ::telemetry::descriptor::Descriptor<F, C, S, P>{source};
}

constexpr auto bad = withProfile<Profile>(model);
#elif CASE == 10
// More than one scope level is not a type name.
constexpr auto bad = tr::typeName("power::adc::Config");
#elif CASE == 11
// A path character inside a part is not a type name.
constexpr auto bad = tr::typeName("adc/Config");
#else
#error Unknown CASE
#endif
} // namespace negative

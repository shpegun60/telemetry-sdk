/*
 * @file AliasProbe.cpp
 * @brief Measures the two type forms whose automatic name compilers spell differently.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * An unnamed struct given its name for linkage by an alias-declaration
 * (using Alias = struct {...};) is spelled "Alias" by Clang and as an unnamed
 * struct by GCC, MinGW and ARM GCC, whose function signature carries no name
 * to recover; the typedef form is spelled "Typedef" by both. A struct in an
 * inline namespace (probe::v2::Status) is "v2::Status" for GCC, MinGW and ARM
 * GCC and "probe::Status" for Clang, which does not print the inline
 * namespace (plan rule 4: such a type is named explicitly when its model is
 * built with Clang). run.py records what each compiler gives and checks the
 * inline spelling per compiler family; neither form is part of the cross.hex
 * byte fixture.
 */

#include <telemetry/reflection/TypeName.hpp>

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace probe {
using Alias = struct {
	std::uint8_t raw;
};

typedef struct {
	std::uint8_t raw;
} Typedef;

inline namespace v2 {
struct Status {
	std::uint8_t code;
};
} // namespace v2
} // namespace probe

#if defined(CROSS_ARM)
namespace {
template<std::size_t N>
struct Text {
	char bytes[N + 1];
};

template<class T>
consteval auto text() noexcept
{
	constexpr auto name = telemetry::reflection::typeName<T>();
	Text<name.size()> result{};
	for (std::size_t i = 0; i < name.size(); ++i)
		result.bytes[i] = name[i];
	return result;
}
} // namespace

extern "C" {
__attribute__((used, section(".probe.alias"))) const auto alias_probe_alias = text<probe::Alias>();
__attribute__((used, section(".probe.typedef"))) const auto alias_probe_typedef =
    text<probe::Typedef>();
__attribute__((used, section(".probe.inline"))) const auto alias_probe_inline =
    text<probe::Status>();
}
#else
int main()
{
	const auto alias = telemetry::reflection::typeName<probe::Alias>();
	const auto named = telemetry::reflection::typeName<probe::Typedef>();
	const auto inlined = telemetry::reflection::typeName<probe::Status>();
	std::printf("alias=[%.*s] typedef=[%.*s] inline=[%.*s]\n", static_cast<int>(alias.size()),
	            alias.data(), static_cast<int>(named.size()), named.data(),
	            static_cast<int>(inlined.size()), inlined.data());
	return 0;
}
#endif

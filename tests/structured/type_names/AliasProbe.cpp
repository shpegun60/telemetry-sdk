/*
 * @file AliasProbe.cpp
 * @brief Measures the one struct form whose automatic name compilers spell differently.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * An unnamed struct given its name for linkage by an alias-declaration
 * (using Alias = struct {...};) is spelled "Alias" by Clang and as an unnamed
 * struct by GCC, MinGW and ARM GCC, whose function signature carries no name
 * to recover; the typedef form is spelled "Typedef" by both. run.py records
 * what each compiler gives; it is not part of the cross.hex byte fixture.
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
}
#else
int main()
{
	const auto alias = telemetry::reflection::typeName<probe::Alias>();
	const auto named = telemetry::reflection::typeName<probe::Typedef>();
	std::printf("alias=[%.*s] typedef=[%.*s]\n", static_cast<int>(alias.size()), alias.data(),
	            static_cast<int>(named.size()), named.data());
	return 0;
}
#endif

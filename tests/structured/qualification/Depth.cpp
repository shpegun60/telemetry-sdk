/* Compile-time growth with recursive reflected structs, without changing core.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT. */
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>

namespace ts = telemetry;

template<unsigned D>
struct Nested {
	Nested<D - 1> child;
};

template<>
struct Nested<0> {
	std::uint32_t value;
};

Nested<DEPTH> readValue() noexcept
{
	return {};
}

inline constexpr ts::Model model{ts::group("depth", ts::field<&readValue>("nested"))};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = ::telemetry::descriptor::packDescriptor<descriptor>();
static_assert(ts::wireSize<Nested<DEPTH>> == 4);

extern "C" unsigned depth_byte(unsigned index) noexcept
{
	return index < bytes.size() ? std::to_integer<unsigned>(bytes[index]) : 0;
}

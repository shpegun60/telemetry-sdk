/* Actual native Model for generated/static ReadMany qualification. MIT. */
#ifndef TELEMETRY_TESTS_READ_MANY_STATIC_MODEL_HPP
#define TELEMETRY_TESTS_READ_MANY_STATIC_MODEL_HPP
#pragma once

#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace static_many_fixture {
namespace ts = telemetry;

enum class Mode : std::int16_t {
	Reverse = -7,
	Forward = 12
};

struct Record {
	std::uint32_t value;
};

inline constexpr std::array<const char*, 32> names{
    "F00", "F01", "F02", "F03", "F04", "F05", "F06", "F07", "F08", "F09", "F10",
    "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21",
    "F22", "F23", "F24", "F25", "F26", "F27", "F28", "F29", "F30", "F31"};

inline constexpr std::uint32_t word(std::size_t index) noexcept
{
	return 0x10203040u ^ (static_cast<std::uint32_t>(index) * 0x01010101u);
}

struct Owner {
	std::array<unsigned, 13> mixedCalls{};
	std::array<unsigned, 32> scalarCalls{};
	unsigned ordinaryCalls = 0;
	std::uint32_t f32Bits = 0xff812345u;
	std::uint64_t f64Bits = UINT64_C(0x8000000000000000);

	std::uint32_t ordinary() noexcept
	{
		++ordinaryCalls;
		return 0xa1b2c3d4u;
	}

	bool flag() noexcept
	{
		++mixedCalls[0];
		return true;
	}

	std::uint8_t u8() noexcept
	{
		++mixedCalls[1];
		return UINT8_MAX;
	}

	std::int8_t s8() noexcept
	{
		++mixedCalls[2];
		return INT8_MIN;
	}

	std::uint16_t u16() noexcept
	{
		++mixedCalls[3];
		return UINT16_MAX;
	}

	std::int16_t s16() noexcept
	{
		++mixedCalls[4];
		return INT16_MIN;
	}

	std::uint32_t u32() noexcept
	{
		++mixedCalls[5];
		return UINT32_MAX;
	}

	std::int32_t s32() noexcept
	{
		++mixedCalls[6];
		return INT32_MIN;
	}

	std::uint64_t u64() noexcept
	{
		++mixedCalls[7];
		return UINT64_MAX;
	}

	std::int64_t s64() noexcept
	{
		++mixedCalls[8];
		return INT64_MIN;
	}

	float f32() noexcept
	{
		++mixedCalls[9];
		return std::bit_cast<float>(f32Bits);
	}

	double f64() noexcept
	{
		++mixedCalls[10];
		return std::bit_cast<double>(f64Bits);
	}

	Mode mode() noexcept
	{
		++mixedCalls[11];
		return static_cast<Mode>(-123);
	}

	Record record() noexcept
	{
		++mixedCalls[12];
		return {0x55aa33ccu};
	}

	template<std::size_t I>
	std::uint32_t scalar() noexcept
	{
		++scalarCalls[I];
		return word(I);
	}

	void reset() noexcept
	{
		mixedCalls.fill(0);
		scalarCalls.fill(0);
		ordinaryCalls = 0;
		f32Bits = 0xff812345u;
		f64Bits = UINT64_C(0x8000000000000000);
	}

	unsigned calls() const noexcept
	{
		unsigned total = ordinaryCalls;
		for (auto count : mixedCalls)
			total += count;
		for (auto count : scalarCalls)
			total += count;
		return total;
	}
};

inline Owner owner;
inline ts::OwnerSlot<Owner> absent;

template<std::size_t Offset, std::size_t... I>
constexpr auto scalarGroup(const char* name, std::index_sequence<I...>) noexcept
{
	return ts::group(name,
	                 ts::field<&Owner::template scalar<Offset + I>>(names[Offset + I], owner)...);
}

// Four Fields-only groups: ordinary, mixed and two of sixteen scalars.
inline constexpr ts::Model model{
    ts::group("ordinary", ts::field<&Owner::ordinary>("Value", owner)),
    ts::group("mixed", ts::field<&Owner::flag>("Bool", owner), ts::field<&Owner::u8>("U8", owner),
              ts::field<&Owner::s8>("S8", owner), ts::field<&Owner::u16>("U16", owner),
              ts::field<&Owner::s16>("S16", owner), ts::field<&Owner::u32>("U32", owner),
              ts::field<&Owner::s32>("S32", owner), ts::field<&Owner::u64>("U64", owner),
              ts::field<&Owner::s64>("S64", owner), ts::field<&Owner::f32>("F32", owner),
              ts::field<&Owner::f64>("F64", owner), ts::field<&Owner::mode>("Enum", owner),
              ts::field<&Owner::record>("Struct", owner),
              ts::field<&Owner::u64>("Unavailable", absent)),
    scalarGroup<0>("left", std::make_index_sequence<16>{}),
    scalarGroup<16>("right", std::make_index_sequence<16>{})};
inline constexpr ts::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = ts::descriptor::packDescriptor<descriptor>();
} // namespace static_many_fixture
#endif

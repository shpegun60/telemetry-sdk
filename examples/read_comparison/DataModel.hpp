/* Identical immutable values exposed by scalar or alternative aggregate Models. MIT. */
#ifndef TELEMETRY_EXAMPLES_READ_COMPARISON_DATA_MODEL_HPP
#define TELEMETRY_EXAMPLES_READ_COMPARISON_DATA_MODEL_HPP
#pragma once
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <array>
#include <cstdint>
#include <utility>

namespace read_comparison {
namespace ts = telemetry;
inline constexpr auto data = [] {
	std::array<std::uint32_t, 32> result{};
	for (std::size_t i = 0; i < result.size(); ++i)
		result[i] = 0x10203040u ^ (std::uint32_t(i) * 0x01010101u);
	result[0] = 0;
	result[7] = UINT32_MAX;
	return result;
}();
inline constexpr std::array<const char*, 32> names{
    "F00", "F01", "F02", "F03", "F04", "F05", "F06", "F07", "F08", "F09", "F10",
    "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21",
    "F22", "F23", "F24", "F25", "F26", "F27", "F28", "F29", "F30", "F31"};

struct Bundle {
	std::array<std::uint32_t, 8> values;
};

template<std::size_t Count, std::size_t Stride>
struct DataModel {
	static_assert(Count <= 32 && 7 * Stride < Count);
	static constexpr std::size_t fieldCount = Count;
	static constexpr auto selected = [] {
		std::array<std::uint32_t, 8> ids{};
		for (std::size_t i = 0; i < ids.size(); ++i)
			ids[i] = std::uint32_t(i * Stride);
		return ids;
	}();

	struct Owner {
		std::array<std::uint64_t, Count> scalarCalls{};
		std::uint64_t aggregateCalls = 0;

		template<std::size_t I>
		std::uint32_t read() noexcept
		{
			++scalarCalls[I];
			return data[I];
		}

		Bundle aggregate() noexcept
		{
			++aggregateCalls;
			Bundle result{};
			for (std::size_t i = 0; i < 8; ++i)
				result.values[i] = data[selected[i]];
			return result;
		}

		void reset() noexcept
		{
			scalarCalls.fill(0);
			aggregateCalls = 0;
		}

		std::uint64_t calls() const noexcept
		{
			auto total = aggregateCalls;
			for (auto value : scalarCalls)
				total += value;
			return total;
		}
	};

	static inline Owner owner;

	template<std::size_t... I>
	static constexpr auto makeGroup(std::index_sequence<I...>) noexcept
	{
		return ts::group("data", ts::field<&Owner::template read<I>>(names[I], owner)...);
	}

	// One Fields-only group, data, in two projections of the same owner.
	static inline constexpr ts::Model scalarModel{makeGroup(std::make_index_sequence<Count>{})};
	static inline constexpr ts::descriptor::Descriptor scalarDescriptor{scalarModel};
	static inline constexpr auto scalarBytes = ts::descriptor::packDescriptor<scalarDescriptor>();
	// This is an ALTERNATIVE projection, never a ninth/33rd Field in ValuesFile.
	static inline constexpr ts::Model aggregateModel{
	    ts::group("data", ts::field<&Owner::aggregate>("Snapshot", owner))};
	static inline constexpr ts::descriptor::Descriptor aggregateDescriptor{aggregateModel};
	static inline constexpr auto aggregateBytes =
	    ts::descriptor::packDescriptor<aggregateDescriptor>();
};

using All8 = DataModel<8, 1>;
using Sparse8of32 = DataModel<32, 4>;
} // namespace read_comparison
#endif

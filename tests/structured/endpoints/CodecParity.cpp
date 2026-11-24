/*
 * @file CodecParity.cpp
 * @brief Internal endpoint codec preserves standalone codec bytes and validation.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/model/Model.hpp>
#include <algorithm>
#include <bit>
namespace ts = telemetry;

namespace parity {
enum class Mode : std::uint8_t {
	Off,
	On
};

struct Channel {
	float value;
	bool valid;
};

struct Frame {
	std::uint64_t counter;
	std::array<Channel, 3> channels;
	double energy;
	Mode mode;
};

inline Frame current{};
inline unsigned reads = 0, writes = 0, calls = 0;

Frame get() noexcept
{
	++reads;
	return current;
}

telemetry::WriteResult set(const Frame& value) noexcept
{
	++writes;
	current = value;
	return telemetry::WriteResult::Applied;
}

telemetry::CommandResult call(const Frame& value) noexcept
{
	++calls;
	current = value;
	return telemetry::CommandResult::Executed;
}

// A Field and a Command of one group share ID 0 in their families.
inline constexpr ts::Model model{
    ts::group("p", ts::field<&get, &set>("frame"), ts::command<&call>("call"))};
inline constexpr auto catalogs = model.fieldIndex();
inline constexpr auto actions = model.commandIndex();
} // namespace parity

int main()
{
	using namespace parity;
	constexpr auto count = ts::wireSize<Frame>;
	static_assert(count == 32);
	std::array<std::byte, count + 2> input{}, output{};
	alignas(Frame) std::array<std::byte, ts::scratchBytes<Frame> + 1> backing{};
	auto bytes = std::span{input}.subspan(1, count);
	auto encoded = std::span{output}.subspan(1, count);
	ts::Workspace workspace{std::span{backing}.subspan(1)};
	for (std::uint32_t i = 0; i != 128; ++i) {
		Frame original{UINT64_MAX - i,
		               {{{std::bit_cast<float>(0x7fc00000u + i), bool(i & 1)},
		                 {std::bit_cast<float>(0x80000000u + i), true},
		                 {std::bit_cast<float>(0x7f800000u - i), false}}},
		               std::bit_cast<double>(UINT64_C(0xfff8000000000000) + i),
		               static_cast<Mode>(static_cast<std::uint8_t>(i))};
		if (ts::encode(original, bytes) != ts::CodecStatus::Ok)
			return 1;
		if (catalogs.writeEncoded(0, bytes, workspace).dispatch != ts::DispatchStatus::Ok ||
		    actions.executeEncoded(0, bytes, workspace).dispatch != ts::DispatchStatus::Ok)
			return 2;
		const auto result = catalogs.readEncoded(0, encoded, workspace);
		if (result.dispatch != ts::DispatchStatus::Ok || result.written != count ||
		    !std::equal(bytes.begin(), bytes.end(), encoded.begin()) || workspace.used() != 0)
			return 3;
		for (std::size_t boolOffset : {12u, 17u, 22u}) {
			const auto saved = bytes[boolOffset];
			bytes[boolOffset] = std::byte{255};
			const auto beforeWrites = writes, beforeCalls = calls;
			if (catalogs.writeEncoded(0, bytes, workspace).dispatch !=
			        ts::DispatchStatus::InvalidPayload ||
			    actions.executeEncoded(0, bytes, workspace).dispatch !=
			        ts::DispatchStatus::InvalidPayload ||
			    writes != beforeWrites || calls != beforeCalls || workspace.used() != 0)
				return 4;
			bytes[boolOffset] = saved;
		}
	}
	return reads == 128 && writes == 128 && calls == 128 ? 0 : 5;
}

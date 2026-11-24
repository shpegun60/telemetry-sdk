/**
 * @file PayloadBoundary.cpp
 * @brief Fixed-seed encoded payload checks with independent callback oracles.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Every public compiled family boundary receives the same aggregate wire shape.
 * Length and canonical bool checks precede callbacks; unknown enum codes remain
 * representable values. No raw dispatcher thunk or application race is tested.
 */
#include <telemetry/Telemetry.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace ts = telemetry;
using DS = ts::DispatchStatus;

namespace {
unsigned checks = 0;
unsigned packets = 0;

#define CHECK(...)                                                                                 \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(__VA_ARGS__)) {                                                                      \
			std::fprintf(stderr, "packet %u, line %d: %s\n", packets, __LINE__, #__VA_ARGS__);     \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

enum class Mode : std::uint8_t {
	Off = 0,
	On = 7
};

struct Request {
	std::uint32_t sequence;
	bool enabled;
	std::array<bool, 2> switches;
	Mode mode;
	std::uint16_t sample;
};

static_assert(ts::wireSize<Request> == 10);

// Count application entry and retain exact native data for independent checks.
// Public methods: get() read; set() write; execute() command; echo() service.
struct Device {
	Request last{0x87654321, true, {false, true}, Mode::On, 0x3456};
	unsigned reads = 0, writes = 0, commands = 0, services = 0;

	Request get() noexcept
	{
		++reads;
		return last;
	}

	ts::WriteResult set(const Request& value) noexcept
	{
		++writes;
		last = value;
		return ts::WriteResult::Applied;
	}

	ts::CommandResult execute(const Request& value) noexcept
	{
		++commands;
		last = value;
		return ts::CommandResult::Executed;
	}

	Request echo(const Request& value) noexcept
	{
		++services;
		return value;
	}
};

bool same(const Request& a, const Request& b)
{
	return a.sequence == b.sequence && a.enabled == b.enabled && a.switches == b.switches &&
	       a.mode == b.mode && a.sample == b.sample;
}

unsigned byte(std::span<const std::byte> input, std::size_t i)
{
	return std::to_integer<unsigned>(input[i]);
}

// The expected native members are decoded directly, without Codec or PFR.
Request native(std::span<const std::byte> input)
{
	return {byte(input, 0) | (byte(input, 1) << 8) | (byte(input, 2) << 16) |
	            (std::uint32_t(byte(input, 3)) << 24),
	        byte(input, 4) == 1,
	        {byte(input, 5) == 1, byte(input, 6) == 1},
	        static_cast<Mode>(byte(input, 7)),
	        static_cast<std::uint16_t>(byte(input, 8) | (byte(input, 9) << 8))};
}

std::uint32_t next(std::uint32_t& seed)
{
	seed = seed * 1664525u + 1013904223u;
	return seed;
}

void corpus()
{
	Device device;
	// A runtime tree over a local owner: one group of all three families.
	ts::Model model{ts::group("device", ts::field<&Device::get, &Device::set>("value", device),
	                          ts::command<&Device::execute>("execute", device),
	                          ts::service<&Device::echo>("echo", device))};
	const auto view = model.view();
	alignas(std::max_align_t) std::array<std::byte, 256> storage{};
	ts::Workspace workspace{storage};
	// Nested calls must release their scratch without disturbing an outer lease.
	auto outer = workspace.reserve<std::uint64_t>();
	CHECK(outer.constructFrom([]() noexcept {
		return UINT64_C(0x123456789abcdef0);
	}) != nullptr);
	const auto used = workspace.used();
	unsigned validPackets = 0, unknownEnums = 0;

	const auto inspect = [&](std::span<const std::byte> input, std::size_t capacity) {
		++packets;
		std::array<std::byte, 32> original{};
		std::copy(input.begin(), input.end(), original.begin());
		const bool valid =
		    input.size() == 10 && byte(input, 4) <= 1 && byte(input, 5) <= 1 && byte(input, 6) <= 1;
		const auto expected = valid ? DS::Ok : DS::InvalidPayload;
		const auto before = device.last;
		const auto reads = device.reads, writes = device.writes, commandCalls = device.commands,
		           calls = device.services;
		// Observe each family's result before the next callback can overwrite it.
		const auto write = ts::writeFieldEncoded(view, 0, input, workspace);
		CHECK(write.dispatch == expected);
		CHECK(device.writes == writes + valid);
		CHECK(device.reads == reads && device.commands == commandCalls && device.services == calls);
		CHECK(workspace.used() == used);
		if (valid) {
			CHECK(write.endpointStatus == ts::WriteResult::Applied);
			CHECK(same(device.last, native(input)));
		} else
			CHECK(same(device.last, before));

		const auto command = ts::executeCommandEncoded(view, 0, input, workspace);
		CHECK(command.dispatch == expected);
		CHECK(device.commands == commandCalls + valid);
		CHECK(device.reads == reads && device.writes == writes + valid && device.services == calls);
		CHECK(workspace.used() == used);
		if (valid) {
			CHECK(command.endpointStatus == ts::CommandResult::Executed);
			CHECK(same(device.last, native(input)));
			++validPackets;
			unknownEnums += byte(input, 7) != 0 && byte(input, 7) != 7;
		} else
			CHECK(same(device.last, before));
		const auto afterCommand = device.last;

		std::array<std::byte, 16> output;
		output.fill(std::byte{0xa5});
		const auto serviceExpected = input.size() != 10 ? DS::InvalidPayload
		                             : capacity < 10    ? DS::BufferTooSmall
		                             : valid            ? DS::Ok
		                                                : DS::InvalidPayload;
		const auto service = ts::callServiceEncoded(
		    view, 0, input, std::span(output).subspan(1, capacity), workspace);
		CHECK(service.dispatch == serviceExpected);
		CHECK(device.services == calls + (serviceExpected == DS::Ok));
		CHECK(serviceExpected != DS::Ok || service.endpointStatus == ts::ServiceStatus::Ok);
		CHECK(device.reads == reads && device.writes == writes + valid &&
		      device.commands == commandCalls + valid);
		CHECK(same(device.last, afterCommand));
		CHECK(workspace.used() == used);
		CHECK(service.written == (serviceExpected == DS::Ok ? 10u : 0u));
		for (std::size_t i = 0; i < output.size(); ++i) {
			const auto wanted =
			    serviceExpected == DS::Ok && i >= 1 && i <= 10 ? input[i - 1] : std::byte{0xa5};
			CHECK(output[i] == wanted);
		}
		const auto callbacks = device.reads + device.writes + device.commands + device.services;
		const auto previous = output;
		CHECK(ts::writeFieldEncoded(view, 1, input, workspace).dispatch == DS::NotFound);
		CHECK(ts::executeCommandEncoded(view, 1, input, workspace).dispatch == DS::NotFound);
		CHECK(ts::callServiceEncoded(view, 1, input, output, workspace).dispatch == DS::NotFound);
		CHECK(device.reads + device.writes + device.commands + device.services == callbacks);
		CHECK(output == previous && workspace.used() == used);
		CHECK(std::equal(input.begin(), input.end(), original.begin()));
	};

	std::array<std::byte, 32> seedBytes{};
	seedBytes[0] = std::byte{0x91};
	seedBytes[4] = seedBytes[6] = std::byte{1};
	seedBytes[7] = std::byte{7};
	seedBytes[9] = std::byte{0xff};
	for (std::size_t length = 0; length <= 12; ++length)
		for (std::size_t capacity = 0; capacity <= 13; ++capacity)
			inspect(std::span(seedBytes).first(length), capacity);
	// Every possible byte in every position: three bool members independently
	// reject 2..255, while every underlying enum code is accepted.
	for (std::size_t offset = 0; offset < 10; ++offset) {
		for (unsigned value = 0; value <= 255; ++value) {
			auto changed = seedBytes;
			changed[offset] = std::byte(value);
			inspect(std::span(changed).first(10), 10);
		}
	}
	std::uint32_t seed = 0xf286123a;
	for (unsigned trial = 0; trial < 8000; ++trial) {
		for (auto& item : seedBytes)
			item = std::byte(next(seed) >> 24);
		std::size_t length = next(seed) % 13;
		if (trial % 3 == 0) {
			length = 10;
			seedBytes[4] = std::byte(next(seed) % 2);
			seedBytes[5] = std::byte(next(seed) % 2);
			seedBytes[6] = std::byte(next(seed) % 2);
		}
		inspect(std::span(seedBytes).first(length), next(seed) % 14);
	}
	CHECK(validPackets > 4000 && unknownEnums > 2000);
	CHECK(device.writes == validPackets && device.commands == validPackets);
	CHECK(device.services > 2000);
	CHECK(workspace.used() == used);
	CHECK(outer.get() && *outer.get() == UINT64_C(0x123456789abcdef0));

	// A short read never invokes the getter and never modifies output bytes.
	for (std::size_t capacity = 0; capacity <= 13; ++capacity) {
		std::array<std::byte, 16> output;
		output.fill(std::byte{0xa5});
		const auto reads = device.reads;
		const auto read =
		    ts::readFieldEncoded(view, 0, std::span(output).subspan(1, capacity), workspace);
		CHECK(read.dispatch == (capacity < 10 ? DS::BufferTooSmall : DS::Ok));
		CHECK(device.reads == reads + (capacity >= 10));
		CHECK(read.written == (capacity < 10 ? 0u : 10u));
		CHECK(output.front() == std::byte{0xa5} && output.back() == std::byte{0xa5});
		if (capacity >= 10)
			CHECK(same(native(std::span(output).subspan(1, 10)), device.last));
		else
			CHECK(std::all_of(output.begin(), output.end(), [](auto value) {
				return value == std::byte{0xa5};
			}));
		CHECK(workspace.used() == used);
	}
	std::printf("Encoded corpus: %u packets; %u canonical; %u unknown enum codes\n", packets,
	            validPackets, unknownEnums);
}
} // namespace

int main()
{
	corpus();
	std::printf("Encoded boundaries: %u checks passed\n", checks);
}

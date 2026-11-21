/*
 * @file StoragePolicyProbe.cpp
 * @brief Storage boundaries, mixed Service lifetimes and scratch accounting.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/model/Model.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ts = telemetry;

template<std::size_t Bytes>
struct Payload {
	std::array<std::uint8_t, Bytes> bytes;
};

// Sized payload owner exposes Field and Command storage requirements at budget boundaries.
// API: read(), write(), command().
template<std::size_t Bytes>
struct Device {
	Payload<Bytes> value{};
	unsigned calls = 0;

	Payload<Bytes> read() noexcept
	{
		++calls;
		return value;
	}

	telemetry::WriteResult write(const Payload<Bytes>& next) noexcept
	{
		++calls;
		value = next;
		return telemetry::WriteResult::Applied;
	}

	telemetry::CommandResult command(const Payload<Bytes>& next) noexcept
	{
		++calls;
		value = next;
		return telemetry::CommandResult::Executed;
	}
};

template<std::size_t Bytes>
int checkValue()
{
	using Value = Payload<Bytes>;
	static_assert(sizeof(Value) == Bytes);
	Device<Bytes> device;
	ts::FieldTable fields{ts::field<&Device<Bytes>::read, &Device<Bytes>::write>("Value", device)};
	ts::CommandTable commands{ts::command<&Device<Bytes>::command>("Set", device)};
	// The same endpoints in one group of a runtime tree: both IDs are 0.
	ts::Model model{
	    ts::group("device", ts::field<&Device<Bytes>::read, &Device<Bytes>::write>("Value", device),
	              ts::command<&Device<Bytes>::command>("Set", device))};
	constexpr bool local = Bytes <= ts::maxLocalObjectBytes;
	constexpr auto expectedScratch = local ? 0 : ts::scratchBytes<Value>;
	if (fields.data()[0].readScratchBytes != expectedScratch ||
	    fields.data()[0].writeScratchBytes != expectedScratch ||
	    commands.data()[0].scratchBytes != expectedScratch)
		return 1;

	std::array<std::byte, Bytes> input{};
	input.front() = std::byte{31};
	input.back() = std::byte{7};
	std::array<std::byte, Bytes> output{};
	ts::Workspace empty{std::span<std::byte>{}};
	constexpr auto expected =
	    local ? ts::DispatchStatus::Ok : ts::DispatchStatus::WorkspaceTooSmall;
	if (model.fieldIndex().writeEncoded(0u, input, empty).dispatch != expected ||
	    model.fieldIndex().readEncoded(0u, output, empty).dispatch != expected ||
	    model.commandIndex().executeEncoded(0u, input, empty).dispatch != expected)
		return 2;
	if (device.calls != (local ? 3u : 0u) || empty.used() != 0)
		return 3;

	// Keep another lease alive while dispatch uses and releases its own objects.
	// This also tests nonzero incoming Workspace usage for both storage choices.
	std::array<std::byte, ts::scratchBytes<Value> + 16> memory{};
	ts::Workspace workspace{memory};
	auto outer = workspace.reserve<std::uint8_t>();
	if (!outer.valid())
		return 4;
	*outer.constructDefault() = 91;
	const auto mark = workspace.used();
	if (model.fieldIndex().writeEncoded(0u, input, workspace).dispatch != ts::DispatchStatus::Ok ||
	    model.commandIndex().executeEncoded(0u, input, workspace).dispatch !=
	        ts::DispatchStatus::Ok ||
	    model.fieldIndex().readEncoded(0u, output, workspace).dispatch != ts::DispatchStatus::Ok ||
	    input != output || workspace.used() != mark)
		return 5;
	return 0;
}

// Responder pairs sized Request and Response payloads to measure their simultaneous storage.
// API: call().
template<std::size_t RequestBytes, std::size_t ResponseBytes>
struct Responder {
	unsigned calls = 0;

	Payload<ResponseBytes> call(const Payload<RequestBytes>& input) noexcept
	{
		++calls;
		Payload<ResponseBytes> result{};
		result.bytes.front() = input.bytes.front();
		result.bytes.back() = input.bytes.back();
		return result;
	}
};

template<std::size_t RequestBytes, std::size_t ResponseBytes>
int checkService()
{
	using Request = Payload<RequestBytes>;
	using Result = ts::ServiceResult<Payload<ResponseBytes>>;
	Responder<RequestBytes, ResponseBytes> owner;
	ts::Model model{ts::group(
	    "services", ts::service<&Responder<RequestBytes, ResponseBytes>::call>("Call", owner))};

	// The independent expectation uses the actual wrapper, not just Response.
	constexpr bool requestLocal = sizeof(Request) <= ts::maxLocalObjectBytes;
	constexpr auto usedLocal = requestLocal ? sizeof(Request) : 0;
	constexpr bool resultLocal = sizeof(Result) <= ts::maxLocalObjectBytes - usedLocal;
	constexpr auto required =
	    (requestLocal ? 0 : ts::scratchBytes<Request>)+(resultLocal ? 0 : ts::scratchBytes<Result>);
	if (model.maxServiceScratch() != required)
		return 6;
	std::array<std::byte, RequestBytes> input{};
	input.front() = std::byte{41};
	input.back() = std::byte{5};
	std::array<std::byte, ResponseBytes> output{};
	std::array<std::byte, required + 16> memory{};
	if constexpr (required > 0) {
		ts::Workspace shortWorkspace{std::span{memory}.first(required - 1)};
		const auto original = output;
		if (model.serviceIndex().callEncoded(0u, input, output, shortWorkspace).dispatch !=
		        ts::DispatchStatus::WorkspaceTooSmall ||
		    owner.calls != 0 || output != original || shortWorkspace.used() != 0)
			return 7;
	}
	ts::Workspace workspace{std::span{memory}.first(required)};
	const auto result = model.serviceIndex().callEncoded(0u, input, output, workspace);
	if (result.dispatch != ts::DispatchStatus::Ok ||
	    result.endpointStatus != ts::ServiceStatus::Ok || result.written != ResponseBytes ||
	    owner.calls != 1 || workspace.used() != 0 || output.front() != input.front() ||
	    output.back() != input.back())
		return 8;
	return 0;
}

struct alignas(32) Aligned {
	std::uint32_t value;
};

Aligned readAligned() noexcept
{
	return {0x12345678u};
}

telemetry::WriteResult writeAligned(const Aligned& value) noexcept
{
	return value.value == 0x12345678u ? telemetry::WriteResult::Applied
	                                  : telemetry::WriteResult::InvalidValue;
}

int checkAlignment()
{
	ts::Model model{ts::group("test", ts::field<&readAligned, &writeAligned>("Aligned"))};
	std::array<std::byte, 4> bytes{};
	std::array<std::byte, ts::scratchBytes<Aligned> + 32> storage{};
	for (std::size_t offset = 0; offset < 32; ++offset) {
		ts::Workspace workspace{std::span{storage}.subspan(offset, ts::scratchBytes<Aligned>)};
		const auto read = model.fieldIndex().readEncoded(0u, bytes, workspace);
		const auto write = model.fieldIndex().writeEncoded(0u, bytes, workspace);
		if (read.dispatch != ts::DispatchStatus::Ok || write.dispatch != ts::DispatchStatus::Ok ||
		    write.endpointStatus != telemetry::WriteResult::Applied || workspace.used() != 0)
			return 9;
	}
	return 0;
}

int main()
{
	// Exact boundary and next byte, plus large objects that must stay borrowed.
	for (const auto result :
	     {checkValue<1>(), checkValue<15>(), checkValue<16>(), checkValue<17>(), checkValue<31>(),
	      checkValue<32>(), checkValue<33>(), checkValue<63>(), checkValue<64>(), checkValue<65>(),
	      checkValue<4096>(), checkService<4, 4>(), checkService<16, 16>(), checkService<32, 32>(),
	      checkService<64, 64>(), checkService<65, 4>(), checkService<4, 65>(), checkAlignment()}) {
		if (result != 0)
			return result;
	}
	return 0;
}

/*
 * @file Mixed.cpp
 * @brief Small/large native and encoded probes plus descriptor/values reads.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
#include "Fixture.hpp"
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <algorithm>

#ifndef MCU_SCALE
namespace mcu {
namespace {
using namespace fixture;
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
// Cache borrowed views as a real transport consumer does. Constructing a
// temporary ModelView for every probe would measure test setup as dispatch.
inline constexpr auto view = model.view();
inline constexpr rs::DescriptorFile descriptorFile{descriptor};
inline constexpr rs::ValuesFile values{descriptor, qualification::sharedWorkspace};
alignas(32) std::array<std::byte, 16384> output;
std::array<std::byte, ts::wireSize<Big>> input;
constexpr std::array<std::byte, 5> smallInput{std::byte{17}, std::byte{0}, std::byte{0},
                                              std::byte{0}, std::byte{1}};

// These large-request endpoints are independent of the published Stage 13
// fixture. They add real 4 KiB input coverage without changing its 97 checks.
struct LargeDevice {
	unsigned commands = 0, services = 0;
	std::uint32_t last = 0;

	telemetry::CommandResult accept(const Big& request) noexcept
	{
		++commands;
		last = request.words.back();
		return telemetry::CommandResult::Executed;
	}

	ts::ServiceResult<Big> echo(const Big& request) noexcept
	{
		++services;
		return ts::ServiceResult<Big>::successFrom([&]() noexcept {
			return request;
		});
	}
};

LargeDevice largeDevice;
inline constexpr ts::Model largeModel{
    ts::group("large", ts::command<&LargeDevice::accept>("Accept", largeDevice),
              ts::service<&LargeDevice::echo>("Echo", largeDevice))};
inline constexpr auto largeView = largeModel.view();

// The two populated groups select the same owner through different catalog
// positions; the empty middle group remains available to correctness tests.
std::uint32_t id(unsigned inputId, unsigned entry) noexcept
{
	return ((inputId & 1u) ? 0x20000u : 0u) | entry;
}

std::uint32_t word() noexcept
{
	std::uint32_t result = 0;
	for (unsigned i = 0; i != 4; ++i)
		result |= std::uint32_t(std::to_integer<unsigned>(output[i])) << (i * 8);
	return result;
}

std::uint32_t direct(std::uint32_t) noexcept
{
	return device.readConfig().code;
}

std::uint32_t local(std::uint32_t) noexcept
{
	return localFields.read<7>()->code;
}

std::uint32_t global(std::uint32_t) noexcept
{
	return fields.read<7>()->code;
}

std::uint32_t slotRead(std::uint32_t) noexcept
{
	return localFields.read<9>()->code;
}

struct ConfigVisitor {
	std::uint32_t* result;

	template<class Endpoint>
	void operator()(const Endpoint& endpoint) const noexcept
	{
		if constexpr (std::is_same_v<typename Endpoint::Value, Config>)
			if (const auto value = endpoint.read())
				*result = value->code;
	}
};

std::uint32_t visitor(std::uint32_t inputId) noexcept
{
	std::uint32_t result = 0;
	(void)fields.visit(id(inputId, 7), ConfigVisitor{&result});
	return result;
}

std::uint32_t readAs(std::uint32_t inputId) noexcept
{
	const auto value = fields.readAs<Config>(id(inputId, 7));
	return value ? value->code : 0;
}

std::uint32_t readEncoded(std::uint32_t inputId) noexcept
{
	const auto result = ts::readFieldEncoded(view, id(inputId, 7), std::span{output}.first(5),
	                                         qualification::sharedWorkspace);
	return result.dispatch == ts::DispatchStatus::Ok ? word() : 0;
}

std::uint32_t writeEncoded(std::uint32_t inputId) noexcept
{
	const auto result =
	    ts::writeFieldEncoded(view, id(inputId, 7), smallInput, qualification::sharedWorkspace);
	return result.dispatch == ts::DispatchStatus::Ok && result.endpointStatus == W::Applied;
}

std::uint32_t commandEncoded(std::uint32_t inputId) noexcept
{
	const auto result =
	    ts::executeCommandEncoded(view, id(inputId, 0), smallInput, qualification::sharedWorkspace);
	return result.dispatch == ts::DispatchStatus::Ok && result.endpointStatus == C::Executed;
}

std::uint32_t serviceDirect(std::uint32_t) noexcept
{
	return device.echo(Config{17, true}).code;
}

// A handle and a path reach the same Service definition, the tree's own copy:
// with null checks both load that one owner address.
std::uint32_t serviceLocal(std::uint32_t) noexcept
{
	return ts::endpoint<model, "first/Echo">.call(Config{17, true}).value().code;
}

std::uint32_t serviceGlobal(std::uint32_t) noexcept
{
	return ts::call<model, "first/Echo">(Config{17, true}).value().code;
}

std::uint32_t serviceEncoded(std::uint32_t inputId) noexcept
{
	const auto result =
	    ts::callServiceEncoded(view, id(inputId, 0), smallInput, std::span{output}.first(5),
	                           qualification::sharedWorkspace);
	return result.dispatch == ts::DispatchStatus::Ok ? word() : 0;
}

std::uint32_t bigRead(std::uint32_t) noexcept
{
	const auto result = ts::readFieldEncoded(view, 8u, std::span{output}.first(4096),
	                                         qualification::sharedWorkspace);
	return result.dispatch == ts::DispatchStatus::Ok
	           ? result.written + std::to_integer<unsigned>(output[0]) +
	                 std::to_integer<unsigned>(output[4095])
	           : 0;
}

std::uint32_t bigCommand(std::uint32_t) noexcept
{
	const auto result =
	    ts::executeCommandEncoded(largeView, 0u, input, qualification::sharedWorkspace);
	return result.dispatch == ts::DispatchStatus::Ok && result.endpointStatus == C::Executed;
}

std::uint32_t bigService(std::uint32_t) noexcept
{
	const auto result = ts::callServiceEncoded(largeView, 0u, input, std::span{output}.first(4096),
	                                           qualification::sharedWorkspace);
	return result.dispatch == ts::DispatchStatus::Ok
	           ? result.written + std::to_integer<unsigned>(output[0]) +
	                 std::to_integer<unsigned>(output[4095])
	           : 0;
}

std::uint32_t descriptorChunk(std::uint32_t inputId) noexcept
{
	const auto cursor = inputId % descriptor.size();
	const auto result = descriptorFile.read(cursor, std::span{output}.first(128));
	return result.status == resource::Status::Ok
	           ? result.written + std::to_integer<unsigned>(output[0]) +
	                 std::to_integer<unsigned>(output[result.written - 1])
	           : 0;
}

std::uint32_t packedChunk(std::uint32_t inputId) noexcept
{
	const auto bytes = qualification::descriptorBytes();
	const auto offset = inputId % bytes.size();
	const auto count = std::min<std::size_t>(128, bytes.size() - offset);
	std::copy_n(bytes.begin() + offset, count, output.begin());
	return count + std::to_integer<unsigned>(output[0]) +
	       std::to_integer<unsigned>(output[count - 1]);
}

std::uint32_t valuesRead(std::uint32_t) noexcept
{
	const auto result = values.read(0, output);
	return result.status == resource::Status::Ok
	           ? result.written + std::to_integer<unsigned>(output[0]) +
	                 std::to_integer<unsigned>(output[result.written - 1])
	           : 0;
}

constexpr Operation probes[]{{"direct_config", direct, 1, 4096},
                             {"local_config", local, 1, 4096},
                             {"global_config", global, 1, 4096},
                             {"slot_config", slotRead, 1, 4096},
                             {"visit_config", visitor, 7, 4096},
                             {"readAs_config", readAs, 7, 4096},
                             {"encoded_read", readEncoded, 7, 4096},
                             {"encoded_write", writeEncoded, 7, 4096},
                             {"encoded_command", commandEncoded, 7, 4096},
                             {"direct_service", serviceDirect, 1, 4096},
                             {"local_service", serviceLocal, 1, 4096},
                             {"global_service", serviceGlobal, 1, 4096},
                             {"encoded_service", serviceEncoded, 7, 4096},
                             {"big_read", bigRead, 1, 128},
                             {"big_command", bigCommand, 1, 128},
                             {"big_service", bigService, 1, 128},
                             {"descriptor_chunk", descriptorChunk, 7, 512},
                             {"packed_chunk", packedChunk, 7, 512},
                             {"values", valuesRead, 1, 128}};
} // namespace

std::span<const Operation> operations() noexcept
{
	return probes;
}

void prepare() noexcept
{
	device.config = {17, true};
	slot.bind(device);
	for (unsigned i = 0; i < input.size(); ++i)
		input[i] = std::byte((i * 7) & 255u);
}

std::uint32_t expected(unsigned operation, std::uint32_t inputId) noexcept
{
	if (operation == 7 || operation == 8 || operation == 14)
		return 1;
	if (operation == 13)
		return 4097;
	if (operation == 15)
		return 4345;
	if (operation == 16 || operation == 17) {
		const auto packed = qualification::descriptorBytes();
		const auto offset = inputId % packed.size();
		const auto count = std::min<std::size_t>(128, packed.size() - offset);
		return count + std::to_integer<unsigned>(packed[offset]) +
		       std::to_integer<unsigned>(packed[offset + count - 1]);
	}
	if (operation == 18)
		return values.size() + 'T' + 1;
	return 17;
}

namespace {
// Separate correctness phases keep their test temporaries out of one large
// Debug frame; these helpers are never part of a timed operation.
__attribute__((noinline)) void checkLargeRequests() noexcept
{
	using qualification::check;
	(void)bigService(0);
	check(std::equal(input.begin(), input.end(), output.begin()));
	check(largeDevice.last == 0xf9f2ebe4u);
	check(qualification::sharedWorkspace.used() == 0);
	const auto before = largeDevice.services;
	ts::Workspace empty{std::span<std::byte>{}};
	check(ts::callServiceEncoded(largeView, 0u, input, output, empty).dispatch ==
	      ts::DispatchStatus::WorkspaceTooSmall);
	check(largeDevice.services == before);
	// Large in-place response must match every input byte, not just checksum.
	const auto inplace =
	    ts::callServiceEncoded(largeView, 0u, input, input, qualification::sharedWorkspace);
	check(inplace.dispatch == ts::DispatchStatus::Ok && inplace.written == input.size());
	for (unsigned i = 0; i < input.size(); ++i)
		check(input[i] == std::byte((i * 7) & 255u));
	check(qualification::sharedWorkspace.used() == 0);
}

__attribute__((noinline)) void checkDescriptorSlices() noexcept
{
	using qualification::check;
	for (unsigned cursor = 0; cursor < descriptor.size(); ++cursor) {
		const auto result = descriptorFile.read(cursor, std::span{output}.first(128));
		const auto count = std::min<unsigned>(128, descriptor.size() - cursor);
		const bool valid = result.status == resource::Status::Ok && result.written == count &&
		                   result.next == cursor + count &&
		                   result.eof == (cursor + count == descriptor.size());
		check(valid);
		if (!valid)
			return;
		const auto golden = qualification::descriptorBytes().subspan(cursor, count);
		check(std::equal(golden.begin(), golden.end(), output.begin()));
	}
}
} // namespace

void checkProbes() noexcept
{
	prepare();
	for (unsigned operation = 0; operation < operations().size(); ++operation)
		for (unsigned profile = 0; profile < 3; ++profile)
			if (operations()[operation].profiles & (1u << profile))
				checkWindow(operation, profile);
	checkLargeRequests();
	checkDescriptorSlices();
}
} // namespace mcu
#endif

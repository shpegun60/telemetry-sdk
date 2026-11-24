/*
 * @file Core.cpp
 * @brief Protocol-free dispatcher statuses, storage and callback preflight checks.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include <telemetry/Telemetry.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace ts = telemetry;
using Op = ts::Operation;
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
static unsigned checks = 0;

static void check(bool condition, const char* message)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "encoded dispatch check=%u failed: %s\n", checks, message);
		std::abort();
	}
}

struct Value {
	std::uint32_t count;
	bool enabled;
};

struct Empty {};

struct Owner {
	Value value{17, true};
	mutable unsigned reads = 0;
	unsigned writes = 0, commands = 0, services = 0;
	ts::WriteResult writeStatus = ts::WriteResult::Applied;
	ts::CommandResult commandStatus = ts::CommandResult::Executed;
	ts::ServiceStatus serviceStatus = ts::ServiceStatus::Ok;

	Value read() const noexcept
	{
		++reads;
		return value;
	}

	ts::WriteResult write(const Value& next) noexcept
	{
		++writes;
		if (writeStatus == ts::WriteResult::Applied)
			value = next;
		return writeStatus;
	}

	ts::CommandResult command(const Value&) noexcept
	{
		++commands;
		return commandStatus;
	}

	ts::CommandResult noRequest() noexcept
	{
		++commands;
		return commandStatus;
	}

	ts::ServiceResult<Value> service(const Value& request) noexcept
	{
		++services;
		if (serviceStatus != ts::ServiceStatus::Ok)
			return ts::ServiceResult<Value>::failure(serviceStatus);
		return ts::ServiceResult<Value>::success({request.count + 1, request.enabled});
	}

	ts::ServiceResult<Empty> empty(const Empty&) noexcept
	{
		++services;
		return ts::ServiceResult<Empty>::success({});
	}

	void noResponse() noexcept
	{
		++services;
	}

	unsigned calls() const noexcept
	{
		return reads + writes + commands + services;
	}
};

static Owner owner;
static constexpr ts::Model model{ts::group(
    "device", ts::field<&Owner::read, &Owner::write>("Value", owner),
    ts::field<&Owner::read>("ReadOnly", owner), ts::command<&Owner::command>("Command", owner),
    ts::command<&Owner::noRequest>("NoRequest", owner),
    ts::service<&Owner::service>("Service", owner), ts::service<&Owner::empty>("Empty", owner),
    ts::service<&Owner::noResponse>("Void", owner))};

static auto encoded(Value value)
{
	std::array<std::byte, ts::wireSize<Value>> bytes{};
	check(ts::encode(value, bytes) == ts::CodecStatus::Ok, "canonical fixture encoding");
	return bytes;
}

static void outcome(const ts::EncodedDispatchResult& result, Op operation,
                    ts::DispatchStatus dispatch, unsigned endpoint = 0, unsigned written = 0)
{
	check(result.operation == operation, "operation/family context retained");
	check(result.dispatch == dispatch, "exact dispatch status");
	check(result.endpointStatus == endpoint, "exact family status");
	check(result.written == written, "exact payload extent");
}

int main()
{
	static_assert(ts::wireSize<Value> == 5 && ts::wireSize<Empty> == 0);
	std::array<std::byte, model.maxScratch()> scratch{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, 32> output{};
	const auto request = encoded({42, false});
	const auto expected = encoded({43, false});
	const auto initial = encoded(owner.value);
	const auto dispatch = [&](Op operation, ts::PackedId id = 0, Input input = {},
	                          Output out = {}) {
		return ts::dispatchEncoded(model.view(), operation, id, input, out, workspace);
	};
	outcome(dispatch(Op::FieldRead, 0, {}, output), Op::FieldRead, ts::DispatchStatus::Ok, 0, 5);
	check(std::equal(initial.begin(), initial.end(), output.begin()), "read canonical bytes");
	for (unsigned status = 0; status != 6; ++status) {
		owner.writeStatus = static_cast<ts::WriteResult>(status);
		const auto calls = owner.writes;
		outcome(dispatch(Op::FieldWrite, 0, request), Op::FieldWrite, ts::DispatchStatus::Ok,
		        status);
		check(owner.writes == calls + 1, "one setter invocation");
	}
	owner.writeStatus = ts::WriteResult::Applied;
	check(owner.value.count == 42 && !owner.value.enabled, "applied write updates owner");
	for (unsigned status = 0; status != 8; ++status) {
		owner.commandStatus = static_cast<ts::CommandResult>(status);
		const auto calls = owner.commands;
		outcome(dispatch(Op::Command, 0, request), Op::Command, ts::DispatchStatus::Ok, status);
		check(owner.commands == calls + 1, "one command invocation including Accepted");
	}
	owner.commandStatus = ts::CommandResult::Executed;
	for (unsigned status = 0; status != 5; ++status) {
		owner.serviceStatus = static_cast<ts::ServiceStatus>(status);
		const auto calls = owner.services;
		output.fill(std::byte{0x5a});
		outcome(dispatch(Op::Service, 0, request, output), Op::Service, ts::DispatchStatus::Ok,
		        status, status == 0 ? 5 : 0);
		check(owner.services == calls + 1, "one service invocation");
		if (status == 0)
			check(std::equal(expected.begin(), expected.end(), output.begin()),
			      "service response bytes");
		else
			check(output[0] == std::byte{0x5a}, "service refusal writes no payload");
	}
	owner.serviceStatus = ts::ServiceStatus::Ok;
	outcome(dispatch(Op::Command, 1), Op::Command, ts::DispatchStatus::Ok);
	outcome(dispatch(Op::Service, 1), Op::Service, ts::DispatchStatus::Ok);
	outcome(dispatch(Op::Service, 2), Op::Service, ts::DispatchStatus::Ok);

	// Invalid op/ID, malformed bool and capacity failures cannot enter an owner.
	const auto noEffect = [&](Op operation, ts::PackedId id, Input input, Output out,
	                          ts::DispatchStatus status, unsigned endpoint = 0) {
		const auto before = owner.calls();
		const auto saved = output;
		outcome(dispatch(operation, id, input, out), operation, status, endpoint);
		check(owner.calls() == before, "preflight failure invokes no endpoint");
		check(workspace.used() == 0, "preflight leaves no scratch lease");
		if (out.data() == output.data())
			check(output == saved, "preflight leaves caller output unchanged");
	};
	for (unsigned invalid : {0u, 5u, 255u})
		noEffect(static_cast<Op>(invalid), 0, request, output, ts::DispatchStatus::InvalidPayload);
	for (Op operation : {Op::FieldRead, Op::FieldWrite, Op::Command, Op::Service})
		noEffect(operation, 65536, operation == Op::FieldRead ? Input{} : Input{request}, output,
		         ts::DispatchStatus::NotFound);
	noEffect(Op::FieldRead, 0, request, output, ts::DispatchStatus::InvalidPayload);
	for (Op operation : {Op::FieldRead, Op::Service})
		noEffect(operation, 0, operation == Op::FieldRead ? Input{} : Input{request},
		         Output{output}.first(4), ts::DispatchStatus::BufferTooSmall);
	for (Op operation : {Op::FieldWrite, Op::Command, Op::Service}) {
		noEffect(operation, 0, Input{request}.first(4), output, ts::DispatchStatus::InvalidPayload);
		auto malformed = request;
		malformed[4] = std::byte{2};
		noEffect(operation, 0, malformed, output, ts::DispatchStatus::InvalidPayload);
	}
	// Preserve native error precedence: ReadOnly wins over malformed setter bytes.
	noEffect(Op::FieldWrite, 1, {}, output, ts::DispatchStatus::Ok,
	         static_cast<unsigned>(ts::WriteResult::ReadOnly));
	// Service capacity is checked even if the callback would refuse the request.
	owner.serviceStatus = ts::ServiceStatus::Busy;
	noEffect(Op::Service, 0, request, Output{output}.first(4), ts::DispatchStatus::BufferTooSmall);
	owner.serviceStatus = ts::ServiceStatus::Ok;

	// An exhausted workspace fails before any endpoint, for every family.
	std::array<std::byte, 1> shortScratch{};
	ts::Workspace shortWorkspace{shortScratch};
	for (Op operation : {Op::FieldRead, Op::FieldWrite, Op::Command, Op::Service}) {
		const auto before = owner.calls();
		outcome(ts::dispatchEncoded(model.view(), operation, 0,
		                            operation == Op::FieldRead ? Input{} : Input{request}, output,
		                            shortWorkspace),
		        operation, ts::DispatchStatus::WorkspaceTooSmall);
		check(owner.calls() == before && shortWorkspace.used() == 0,
		      "insufficient scratch invokes no endpoint and releases reservation");
	}
	// Input/output may share storage for Service; decode precedes response writes.
	auto shared = request;
	outcome(dispatch(Op::Service, 0, shared, shared), Op::Service, ts::DispatchStatus::Ok, 0, 5);
	check(shared == expected, "in-place service preserves native overlap support");
	std::array<std::byte, 6> partial{};
	std::copy(request.begin(), request.end(), partial.begin());
	outcome(dispatch(Op::Service, 0, Input{partial}.first(5), Output{partial}.subspan(1)),
	        Op::Service, ts::DispatchStatus::Ok, 0, 5);
	check(std::equal(expected.begin(), expected.end(), partial.begin() + 1),
	      "partial-overlap service decodes before writing");
	const auto beforeOverlap = owner.calls();
	outcome(dispatch(Op::Service, 0, request, workspace.storage()), Op::Service,
	        ts::DispatchStatus::InvalidPayload);
	check(owner.calls() == beforeOverlap && workspace.used() == 0,
	      "scratch overlap rejected before callback");
	owner.commandStatus = static_cast<ts::CommandResult>(255);
	const auto beforeCorrupt = owner.commands;
	outcome(dispatch(Op::Command, 0, request), Op::Command, ts::DispatchStatus::InternalError);
	check(owner.commands == beforeCorrupt + 1,
	      "invalid application status does not imply callback never ran");
	owner.commandStatus = ts::CommandResult::Executed;
	check(workspace.used() == 0, "all successful operations release scratch");
	std::printf("encoded dispatch checks=%u failures=0\n", checks);
}

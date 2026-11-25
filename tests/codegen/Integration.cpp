/*
 * @file Integration.cpp
 * @brief Counted generated DTO interoperability with native Encoded/Resource RPC.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "Device.hpp"
#include <device_api/RpcBindings.hpp>
#include <device_api/EncodedBindings.hpp>
#include <resource/FileSystem.hpp>
#include <resource/protocol/Protocol.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace cf = codegen_fixture;
namespace ex = client_example;
namespace api = device_api;
namespace rpc = resource::telemetry::v3;
using CS = ::telemetry::client::rpc::RpcClientStatus;
using Config = api::device::types::Config;
using Request = api::device::types::InspectRequest;
using Response = api::device::types::InspectResponse;
using Empty = api::edge::types::GoodResponse;
using Status = api::codec::Status;
static unsigned checks = 0, failures = 0;

static void check(bool value, unsigned line) noexcept
{
	++checks;
	if (!value) {
		++failures;
		std::fprintf(stderr, "codegen Integration.cpp:%u (%u)\n", line, checks);
	}
}

#define CHECK(value) check(bool(value), __LINE__)

static std::vector<std::byte> load(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	const std::vector<char> bytes{std::istreambuf_iterator<char>{file}, {}};
	CHECK(bool(file) || file.eof());
	std::vector<std::byte> result(bytes.size());
	for (std::size_t i = 0; i < bytes.size(); ++i)
		result[i] = std::byte(static_cast<unsigned char>(bytes[i]));
	return result;
}

template<class T>
static T value(std::span<const std::byte> bytes)
{
	T result{};
	CHECK(api::codec::decode(bytes, result) == Status::Ok);
	return result;
}

template<class T>
static void compare(std::span<const std::byte> expected, const T& value)
{
	std::array<std::byte, api::codec::wireSize<T>> generated{}, native{};
	CHECK(api::codec::encode(value, generated) == Status::Ok);
	CHECK(telemetry::encode(value, native) == telemetry::CodecStatus::Ok);
	CHECK(generated == native);
	CHECK(expected.size() == generated.size());
	CHECK(std::equal(expected.begin(), expected.end(), generated.begin(), generated.end()));
}

static void canonical(const std::filesystem::path& directory)
{
	const auto descriptor = load(directory / "descriptor.bin");
	CHECK(descriptor.size() >= 64);
	if (descriptor.size() < 64)
		return;
	CHECK(api::schema::matchesHeader(std::span(descriptor).first(64)));
	CHECK(!api::schema::matchesHeader(descriptor));
	for (unsigned i : {0u, 4u, 6u, 8u, 10u, 16u, 23u, 63u}) {
		std::array<std::byte, 64> header{};
		std::copy_n(descriptor.begin(), 64, header.begin());
		header[i] ^= std::byte{1};
		CHECK(!api::schema::matchesHeader(header));
	}
	const auto config = load(directory / "config.bin");
	const auto request = load(directory / "request.bin");
	const auto response = load(directory / "response.bin");
	const auto scalars = load(directory / "scalars.bin");
	const auto empty = load(directory / "empty.bin");
	CHECK(config.size() == 37 && request.size() == 41 && response.size() == 41 &&
	      scalars.size() == 47 && empty.empty());
	if (config.size() != 37 || request.size() != 41 || response.size() != 41 ||
	    scalars.size() != 47 || !empty.empty())
		return;
	compare(config, value<Config>(config));
	compare(request, value<Request>(request));
	compare(response, value<Response>(response));
	compare(scalars, value<api::edge::types::Scalars>(scalars));
	compare(empty, value<Empty>(empty));
	static_assert(!std::is_same_v<Request, Response>);
	static_assert(api::codec::wireSize<Request> == api::codec::wireSize<Response>);
	static_assert(std::is_void_v<api::device::types::PingResponse>);
	static_assert(!std::is_void_v<Empty> && api::codec::wireSize<Empty> == 0);
	auto output = value<Config>(config);
	const auto savedSerial = output.serial;
	auto malformed = config;
	malformed[28] = std::byte{2}; // Config bool follows u64/i64/f32/f64.
	CHECK(api::codec::decode(malformed, output) == Status::InvalidValue);
	CHECK(output.serial == savedSerial && output.enabled);
	CHECK(api::codec::encode(api::device::Reset, {}) == Status::Ok);
	CHECK(api::codec::decodeResponse(api::device::Ping, {}) == Status::Ok);
}

static void direct()
{
	CHECK(api::schema::matchesHeader(std::span(cf::descriptorBytes).first(64)));
	ex::device = {};
	cf::edge = {};
	const auto model = cf::model.view();
	std::array<std::byte, 37> configBytes{};
	std::array<std::byte, 41> requestBytes{};
	std::array<std::byte, 128> outputBytes{};
	Config config{};
	auto result =
	    api::encoded::readField(model, api::device::Config, outputBytes, cf::workspace, config);
	CHECK(result.ok() && result.operation == api::endpoint::Operation::FieldRead &&
	      result.written == 37);
	CHECK(config.serial == UINT64_MAX && config.offset == INT64_MIN);
	config.serial = 123;
	result =
	    api::encoded::writeField(model, api::device::Config, config, configBytes, cf::workspace);
	CHECK(result.ok() && result.operation == api::endpoint::Operation::FieldWrite);
	CHECK(ex::device.config.serial == 123 && ex::device.calls == 1);
	config.serial = 456;
	result =
	    api::encoded::executeCommand(model, api::device::Apply, config, configBytes, cf::workspace);
	CHECK(result.ok() && result.operation == api::endpoint::Operation::Command);
	CHECK(ex::device.config.serial == 456 && ex::device.calls == 2);
	result = api::encoded::executeCommand(model, api::device::Reset, cf::workspace);
	CHECK(result.ok() && ex::device.config.serial == UINT64_MAX && ex::device.calls == 3);
	Request request{config, 4};
	Response response{};
	result = api::encoded::callService(model, api::device::Inspect, request, requestBytes,
	                                   outputBytes, cf::workspace, response);
	CHECK(result.ok() && result.written == 41 && response.config.serial == 456);
	CHECK(response.checksum == (1u + 256u + 65535u) * 4u);
	constexpr std::array refusals{
	    telemetry::ServiceStatus::InvalidArgument, telemetry::ServiceStatus::Busy,
	    telemetry::ServiceStatus::Unavailable, telemetry::ServiceStatus::Failed};
	for (unsigned i = 0; i < refusals.size(); ++i) {
		request.multiplier = i;
		response.checksum = 0x87654321u;
		result = api::encoded::callService(model, api::device::Inspect, request, requestBytes,
		                                   outputBytes, cf::workspace, response);
		CHECK(!result.ok() && result.dispatched &&
		      result.dispatch == telemetry::DispatchStatus::Ok);
		CHECK(result.endpointStatus == unsigned(refusals[i]) && result.written == 0);
		CHECK(response.checksum == 0x87654321u);
	}
	request.multiplier = 4;
	result = api::encoded::callService(model, api::device::Offline, request, requestBytes,
	                                   outputBytes, cf::workspace, response);
	CHECK(!result.ok() && result.dispatch == telemetry::DispatchStatus::Unavailable);
	Empty empty{};
	result = api::encoded::callService(model, api::edge::Good, outputBytes, cf::workspace, empty);
	CHECK(result.ok() && result.written == 0 && cf::edge.calls == 1);
	result = api::encoded::callService(model, api::edge::Fail, outputBytes, cf::workspace, empty);
	CHECK(!result.ok() && result.endpointStatus == unsigned(telemetry::ServiceStatus::Busy) &&
	      result.written == 0);
	CHECK(!result.commandAccepted());
	result = api::encoded::callService(model, api::edge::Consume, empty, {}, cf::workspace);
	CHECK(result.ok() && result.written == 0 && cf::edge.calls == 3);
	result = api::encoded::executeCommand(model, api::edge::Accept, empty, {}, cf::workspace);
	CHECK(!result.ok() && result.endpointStatus == unsigned(telemetry::CommandResult::Accepted));
	CHECK(result.commandAccepted());
	CHECK(result.operation == api::endpoint::Operation::Command && cf::edge.calls == 4);
	result = api::encoded::callService(model, api::device::Ping, cf::workspace);
	CHECK(result.ok() && result.written == 0);
	api::edge::types::Scalars scalars{};
	result =
	    api::encoded::readField(model, api::edge::Scalars, outputBytes, cf::workspace, scalars);
	CHECK(result.ok() && result.written == 47 && scalars.u64 == UINT64_MAX &&
	      scalars.i64 == INT64_MIN);
	std::array<std::byte, 47> scalarBytes{};
	result =
	    api::encoded::writeField(model, api::edge::Scalars, scalars, scalarBytes, cf::workspace);
	CHECK(!result.ok() && result.dispatch == telemetry::DispatchStatus::Ok &&
	      result.endpointStatus == unsigned(telemetry::WriteResult::ReadOnly));
	const auto calls = ex::device.calls;
	result = api::encoded::executeCommand(model, api::device::Apply, config,
	                                      std::span(configBytes).first(36), cf::workspace);
	CHECK(!result.dispatched && result.codec == Status::LengthMismatch &&
	      ex::device.calls == calls);
	auto missing = api::device::Reset;
	missing.id = 0xffffffffu;
	result = api::encoded::executeCommand(model, missing, cf::workspace);
	CHECK(result.dispatched && result.dispatch == telemetry::DispatchStatus::NotFound &&
	      ex::device.calls == calls);
	const auto bad = telemetry::executeCommandEncoded(
	    model, api::device::Apply.id, std::span(configBytes).first(36), cf::workspace);
	CHECK(bad.dispatch == telemetry::DispatchStatus::InvalidPayload && ex::device.calls == calls);
	result = api::encoded::readField(model, api::device::Config,
	                                 std::as_writable_bytes(std::span(&config, 1)), cf::workspace,
	                                 config);
	CHECK(!result.dispatched && result.codec == Status::Overlap);
	result = api::encoded::readField(model, api::device::Config, std::span(outputBytes).first(36),
	                                 cf::workspace, config);
	CHECK(result.dispatch == telemetry::DispatchStatus::BufferTooSmall);
	CHECK(cf::workspace.used() == 0);
}

static void encodedWorkspaceOverlap()
{
	const auto model = cf::model.view();
	alignas(std::max_align_t) std::array<std::byte, 1024> storage{};
	telemetry::Workspace workspace{storage};
	std::array<std::byte, 37> configBytes{};
	std::array<std::byte, 41> requestBytes{};
	std::array<std::byte, 128> outputBytes{};
	Config config{};
	config.serial = 9;
	Request request{config, 4};
	Response response{};
	Empty empty{};
	{
		auto lease = workspace.reserve<std::uint64_t>();
		CHECK(lease.valid());
		auto* const live = lease.constructFrom([] {
			return std::uint64_t{0xaabbcc};
		});
		CHECK(live != nullptr);
		if (!live)
			return;
		const auto used = workspace.used();
		const auto deviceCalls = ex::device.calls, edgeCalls = cf::edge.calls;
		const auto rejected = [&](api::encoded::Outcome result) {
			CHECK(!result.dispatched && !result.ok() && result.codec == Status::Overlap);
			CHECK(*live == 0xaabbcc && workspace.used() == used);
			CHECK(ex::device.calls == deviceCalls && cf::edge.calls == edgeCalls);
		};
		// The wrapper must reject before encoding into a live native Workspace object.
		rejected(api::encoded::writeField(model, api::device::Config, config,
		                                  std::span(storage).first(37), workspace));
		rejected(api::encoded::executeCommand(model, api::device::Apply, config,
		                                      std::span(storage).first(37), workspace));
		rejected(api::encoded::callService(model, api::device::Inspect, request,
		                                   std::span(storage).first(41), outputBytes, workspace,
		                                   response));
		requestBytes.fill(std::byte{0xa5});
		const auto savedRequest = requestBytes;
		rejected(api::encoded::callService(model, api::device::Inspect, request, requestBytes,
		                                   std::span(storage).first(128), workspace, response));
		CHECK(requestBytes == savedRequest); // Output preflight also precedes request encoding.
		rejected(api::encoded::readField(model, api::device::Config, std::span(storage).first(128),
		                                 workspace, config));
		rejected(api::encoded::callService(model, api::edge::Good, std::span(storage).first(1),
		                                   workspace, empty));
		rejected(api::encoded::callService(model, api::edge::Consume, empty,
		                                   std::span(storage).first(1), workspace));
		// A response wire buffer cannot write through the const native request reference.
		rejected(api::encoded::callService(model, api::device::Inspect, request, requestBytes,
		                                   std::as_writable_bytes(std::span(&request, 1)),
		                                   workspace, response));
		CHECK(request.config.serial == 9 && request.multiplier == 4 &&
		      requestBytes == savedRequest);
	}
	CHECK(workspace.used() == 0);
	// Separate wire buffers remain valid with caller-owned native DTOs in live leases.
	{
		auto lease = workspace.reserve<Config>();
		CHECK(lease.valid());
		auto* const native = lease.constructDefault();
		CHECK(native != nullptr);
		if (!native)
			return;
		const auto used = workspace.used();
		auto result =
		    api::encoded::readField(model, api::device::Config, outputBytes, workspace, *native);
		CHECK(result.ok() && workspace.used() == used);
		native->serial = 0x1234;
		result =
		    api::encoded::writeField(model, api::device::Config, *native, configBytes, workspace);
		CHECK(result.ok() && workspace.used() == used && ex::device.config.serial == 0x1234);
	}
	CHECK(workspace.used() == 0);
}

struct Harness {
	telemetry::ModelView model = cf::model.view();
	std::array<std::byte, cf::model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 152> serverRequest{}, clientRequest{};
	std::array<std::byte, 128> serverResult{}, clientResult{};
	std::array<std::byte, 40> packet{}; // First fragment holds only the RPC header.
	std::array<std::byte, 53> reply{};  // Real response fragmentation, including header.
	rpc::RpcMailbox mailbox{model, workspace, serverRequest, serverResult};
	resource::FileSystem<1> files{std::array{resource::file("/rpc", mailbox)}};
	::telemetry::client::rpc::RpcClient client{
	    0, clientRequest, clientResult, packet, {5, 1000, 1, 2}};
	std::uint32_t now = 0, generation = 1;
	unsigned packets = 0;

	bool admit(std::span<const std::byte> header, std::uint32_t nextGeneration) noexcept
	{
		if (!api::schema::matchesHeader(header))
			return false;
		const auto result = client.startSession(nextGeneration);
		if (result == CS::Ok)
			generation = nextGeneration;
		return result == CS::Ok;
	}

	Harness()
	{
		CHECK(admit(std::span(cf::descriptorBytes).first(64), 1));
	}

	auto deliver(::telemetry::client::rpc::RpcClientPacket pending,
	             bool acceptReply = true) noexcept
	{
		CHECK(bool(pending) && pending.generation == generation);
		CHECK(client.onSendAccepted(pending.generation, pending.requestId));
		const auto result = resource::protocol::process(files.view(), pending.bytes, reply);
		CHECK(result.status == resource::Status::Ok);
		++packets;
		if (acceptReply)
			CHECK(client.onReply(std::span(reply).first(result.written), pending.generation,
			                     pending.requestId));
		return result;
	}

	void run() noexcept
	{
		for (unsigned step = 0; client.active() && step < 400; ++step) {
			client.process(now++);
			const auto pending = client.nextPacket();
			if (pending)
				deliver(pending);
			mailbox.process(now);
		}
		CHECK(client.completed());
		CHECK(workspace.used() == 0);
	}
};

static void remote()
{
	ex::device = {};
	cf::edge = {};
	Harness h;
	const auto trust = ::telemetry::client::rpc::trustedRpcMapping;
	CHECK(h.client.beginReadField(api::rpc::binding(api::device::Config, trust)) == CS::Ok);
	h.run();
	Config config{};
	CHECK(h.client.decodeResult(config).status == CS::Ok && config.serial == UINT64_MAX);
	config.serial = 891;
	CHECK(h.client.beginWriteField(api::rpc::binding(api::device::Config, trust), config) ==
	      CS::Ok);
	h.run();
	CHECK(ex::device.config.serial == 891 && ex::device.calls == 1);
	config.serial = 1234;
	CHECK(h.client.beginCommand(api::rpc::binding(api::device::Apply, trust), config) == CS::Ok);
	h.run();
	CHECK(ex::device.config.serial == 1234 && ex::device.calls == 2);
	Request request{config, 4};
	CHECK(h.client.beginService(api::rpc::binding(api::device::Inspect, trust), request) == CS::Ok);
	Response response{};
	CHECK(h.client.decodeResult(response).status == CS::ResultUnavailable);
	h.run();
	CHECK(h.client.decodeResult(response).status == CS::Ok && response.config.serial == 1234);
	Request sameSize{};
	sameSize.multiplier = 0xaabbccddu;
	CHECK(h.client.decodeResult(sameSize).status == CS::WrongType &&
	      sameSize.multiplier == 0xaabbccddu);
	std::array<std::byte, 41> generated{};
	CHECK(api::codec::encode(response, generated) == Status::Ok);
	CHECK(std::equal(generated.begin(), generated.end(), h.client.resultBytes().begin(),
	                 h.client.resultBytes().end()));
	CHECK(h.packets > 10); // Actual forty-byte MTU requests and fragmented READs were exercised.
	for (unsigned multiplier = 0; multiplier < 4; ++multiplier) {
		request.multiplier = multiplier;
		CHECK(h.client.beginService(api::rpc::binding(api::device::Inspect, trust), request) ==
		      CS::Ok);
		h.run();
		CHECK(h.client.completed() &&
		      h.client.diagnostic().dispatch == telemetry::DispatchStatus::Ok);
		CHECK(h.client.diagnostic().endpointStatus != 0 && h.client.resultBytes().empty());
		CHECK(h.client.decodeResult(response).status == CS::ResultUnavailable);
	}
	CHECK(h.client.beginService(api::rpc::binding(api::edge::Fail, trust)) == CS::Ok);
	h.run();
	Empty empty{};
	CHECK(h.client.resultBytes().empty());
	CHECK(h.client.decodeResult(empty).status == CS::ResultUnavailable);
	CHECK(h.client.diagnostic().endpointStatus == unsigned(telemetry::ServiceStatus::Busy));
	CHECK(h.client.beginCommand(api::rpc::binding(api::edge::Accept, trust), empty) == CS::Ok);
	h.run();
	CHECK(h.client.diagnostic().endpointStatus == unsigned(telemetry::CommandResult::Accepted));
	CHECK(h.client.beginService(api::rpc::binding(api::device::Ping, trust)) == CS::Ok);
	h.run();
	CHECK(h.client.resultBytes().empty() && h.client.diagnostic().endpointStatus == 0);
}

static void pendingRetryAndSessions()
{
	ex::device = {};
	cf::edge = {};
	const auto trust = ::telemetry::client::rpc::trustedRpcMapping;
	Harness h;
	CHECK(h.client.beginService(api::rpc::binding(api::edge::Good, trust)) == CS::Ok);
	// Deliver the WRITE and pending READ without letting the application dispatch.
	auto pending = h.client.nextPacket();
	h.deliver(pending);
	pending = h.client.nextPacket();
	h.deliver(pending);
	Empty empty{};
	CHECK(h.mailbox.state() == ::telemetry::protocol::rpc::RpcState::Pending &&
	      cf::edge.calls == 0);
	CHECK(!h.client.completed() && h.client.resultBytes().empty());
	CHECK(h.client.decodeResult(empty).status == CS::ResultUnavailable);
	h.mailbox.process(++h.now);
	h.run();
	CHECK(h.client.decodeResult(empty).status == CS::Ok && cf::edge.calls == 1);

	Harness retry;
	Request request{};
	request.multiplier = 4;
	CHECK(retry.client.beginService(api::rpc::binding(api::device::Inspect, trust), request) ==
	      CS::Ok);
	const auto calls = ex::device.calls;
	bool dropped = false;
	for (unsigned step = 0; !dropped && step < 20; ++step) {
		retry.client.process(retry.now++);
		pending = retry.client.nextPacket();
		CHECK(bool(pending));
		std::array<std::byte, 40> original{};
		std::copy(pending.bytes.begin(), pending.bytes.end(), original.begin());
		// All request fragments are processed, but the final ACK is lost.
		CHECK(retry.client.onSendAccepted(pending.generation, pending.requestId));
		const auto reply =
		    resource::protocol::process(retry.files.view(), pending.bytes, retry.reply);
		CHECK(reply.status == resource::Status::Ok);
		if (retry.mailbox.state() != ::telemetry::protocol::rpc::RpcState::Pending) {
			CHECK(retry.client.onReply(std::span(retry.reply).first(reply.written),
			                           pending.generation, pending.requestId));
			continue;
		}
		dropped = true;
		retry.mailbox.process(retry.now);
		CHECK(ex::device.calls == calls + 1);
		retry.now += 5;
		retry.client.process(retry.now);
		const auto repeated = retry.client.nextPacket();
		CHECK(repeated.requestId == pending.requestId &&
		      repeated.bytes.size() == pending.bytes.size());
		CHECK(std::equal(repeated.bytes.begin(), repeated.bytes.end(), original.begin(),
		                 original.begin() + repeated.bytes.size()));
		retry.deliver(repeated);
		retry.mailbox.process(retry.now);
	}
	CHECK(dropped);
	retry.run();
	CHECK(ex::device.calls == calls + 1);
	Response response{};
	CHECK(retry.client.decodeResult(response).status == CS::Ok);

	Harness lost;
	CHECK(lost.client.beginCommand(api::rpc::binding(api::device::Reset, trust)) == CS::Ok);
	pending = lost.client.nextPacket();
	lost.deliver(pending, false);
	const auto beforeLoss = ex::device.calls;
	lost.client.loseSession(1);
	CHECK(!lost.client.active() && !lost.client.completed());
	CHECK(lost.client.status() == CS::OutcomeUnknown &&
	      lost.client.diagnostic().cause == CS::SessionLost);
	CHECK(!lost.client.onReply({}, 1, pending.requestId));
	lost.mailbox
	    .resetSession(); // Application excludes the old generation before resetting server state.
	auto wrongHeader = api::schema::descriptorHeader;
	wrongHeader[16] ^= std::byte{1};
	CHECK(!lost.admit(wrongHeader, 2));
	CHECK(lost.client.beginCommand(api::rpc::binding(api::device::Reset, trust)) == CS::NoSession);
	CHECK(lost.admit(api::schema::descriptorHeader, 2));
	CHECK(lost.client.beginCommand(api::rpc::binding(api::device::Reset, trust)) == CS::Ok);
	lost.run();
	CHECK(ex::device.calls == beforeLoss + 1);
}

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	canonical(argv[1]);
	direct();
	encodedWorkspaceOverlap();
	remote();
	pendingRetryAndSessions();
	std::printf("codegen integration checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}

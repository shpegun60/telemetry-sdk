/*
 * @file main.cpp
 * @brief Typed asynchronous calls through an ordinary Resource loopback.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Replace the synchronous fake handoff below with a bounded TCP/UART queue.
 * Keep one Resource exchange outstanding per uncorrelated channel; return the
 * queued generation and requestId with the complete reply. No transport send
 * proves remote receipt. Call process() with current time before events.
 */
#include <client/rpc/RpcClient.hpp>
#include <integration/telemetry/v3/RpcMailbox.hpp>
#include <resource/protocol/Protocol.hpp>
#include <telemetry/model/Model.hpp>
#include <algorithm>
#include <array>
#include <cstdio>

namespace rpc = resource::telemetry::v3;
namespace ts = telemetry;

struct Add {
	std::uint32_t amount;
};

struct Report {
	std::uint32_t total;
};

struct Device {
	std::uint32_t total = 0;
	unsigned serviceCalls = 0;

	Report add(const Add& request) noexcept
	{
		++serviceCalls;
		total += request.amount;
		return {total};
	}

	std::uint32_t read() const noexcept
	{
		return total;
	}

	ts::WriteResult write(std::uint32_t value) noexcept
	{
		total = value;
		return ts::WriteResult::Applied;
	}

	ts::CommandResult clear() noexcept
	{
		total = 0;
		return ts::CommandResult::Executed;
	}
};

static Device device;
// One group holds the Field, the Command and the Service side by side.
static constexpr auto branch = ts::group(
    "demo", ts::field<&Device::read, &Device::write>("Total", device),
    ts::command<&Device::clear>("Clear", device), ts::service<&Device::add>("Add", device));
static constexpr ts::Model model{branch};
static constexpr auto view = model.view();
static std::array<std::byte, 28> serverRequest;
static std::array<std::byte, 4> serverResult;
static std::array<std::byte, model.maxScratch()> scratch;
static ts::Workspace workspace{scratch};
static rpc::RpcMailbox mailbox{view, workspace, serverRequest, serverResult};
static resource::FileSystem files{std::array{resource::file("/rpc", mailbox)}};
static std::array<std::byte, 28> clientRequest;
static std::array<std::byte, 4> clientResult;
static std::array<std::byte, 48> outgoing;
static std::array<std::byte, 80> incoming;
static ::telemetry::client::rpc::RpcClient client{
    0, clientRequest, clientResult, outgoing, {4, 200, 1, 3}};

static unsigned checks = 0, failures = 0;
static std::uint32_t clockMs = 0;
static bool dropFirstAck = true;
static bool retryExpected = false;
static unsigned droppedAcks = 0;
static std::array<std::byte, 48> firstPacket{};
static std::size_t firstPacketSize = 0;
static std::uint32_t firstRequestId = 0;

static void check(bool condition) noexcept
{
	++checks;
	if (!condition)
		++failures;
}

static void complete() noexcept
{
	for (unsigned steps = 0; client.active() && steps < 100; ++steps, ++clockMs) {
		const auto now = clockMs;
		client.process(now);
		const auto packet = client.nextPacket();
		if (packet) {
			if (retryExpected) {
				check(packet.requestId == firstRequestId && packet.bytes.size() == firstPacketSize);
				check(std::equal(packet.bytes.begin(), packet.bytes.end(), firstPacket.begin()));
				check(device.serviceCalls == 1);
				retryExpected = false;
			}
			// Handoff ownership is confirmed before a reply is delivered. The
			// borrowed bytes remain stable until onReply accepts this exchange.
			check(client.onSendAccepted(packet.generation, packet.requestId));
			const auto response = resource::protocol::process(files.view(), packet.bytes, incoming);
			check(response.status == resource::Status::Ok);
			if (dropFirstAck && packet.bytes[0] == std::byte(resource::protocol::Op::Write) &&
			    packet.bytes[13] == std::byte{1}) {
				// The server hands off the ACK, then this fake link loses it.
				// Its later replay must retain both the ticket and every byte.
				dropFirstAck = false;
				retryExpected = true;
				++droppedAcks;
				firstPacketSize = packet.bytes.size();
				firstRequestId = packet.requestId;
				std::copy(packet.bytes.begin(), packet.bytes.end(), firstPacket.begin());
			} else {
				check(client.onReply(resource::Input{incoming}.first(response.written),
				                     packet.generation, packet.requestId));
			}
		}
		// This dispatch is deliberately after the final WRITE acknowledgment.
		mailbox.process(now);
	}
	check(client.completed());
}

// The application gates admission using handshake inputs. This example
// supplies a simulated remote fingerprint; it does not fetch a descriptor.
// A deployment provisions its expected shared schema and obtains the offered
// fingerprint through its admitted connection before constructing bindings.
static ::telemetry::client::rpc::RpcClientStatus
admitSharedSchema(std::uint64_t expected, std::uint64_t offered, std::uint32_t generation) noexcept
{
	if (expected != offered)
		return ::telemetry::client::rpc::RpcClientStatus::NoSession;
	return client.startSession(generation);
}

int main()
{
	// These mappings assert the application's shared schema agreement for this
	// session. They do not fetch or validate a remote descriptor. Each ID comes
	// from its endpoint's path; every family numbers its own entries.
	constexpr ::telemetry::client::rpc::ServiceEndpoint<Add, Report> add{
	    ts::idOf<model, "demo/Add">(), ::telemetry::client::rpc::trustedRpcMapping};
	constexpr ::telemetry::client::rpc::FieldEndpoint<std::uint32_t> total{
	    ts::idOf<model, "demo/Total">(), ::telemetry::client::rpc::trustedRpcMapping};
	constexpr ::telemetry::client::rpc::CommandEndpoint<> clear{
	    ts::idOf<model, "demo/Clear">(), ::telemetry::client::rpc::trustedRpcMapping};
	static_assert(add.id == 0 && total.id == 0 && clear.id == 0); // Group 0, entry 0 each.
	constexpr std::uint64_t expectedFingerprint = 0x534348454d410001ULL;
	check(admitSharedSchema(expectedFingerprint, expectedFingerprint ^ 1, 1) ==
	      ::telemetry::client::rpc::RpcClientStatus::NoSession);
	check(client.beginService(add, Add{7}) == ::telemetry::client::rpc::RpcClientStatus::NoSession);
	check(admitSharedSchema(expectedFingerprint, expectedFingerprint, 1) ==
	      ::telemetry::client::rpc::RpcClientStatus::Ok);
	check(client.beginService(add, Add{7}) == ::telemetry::client::rpc::RpcClientStatus::Ok);
	complete();
	check(droppedAcks == 1 && !retryExpected && device.serviceCalls == 1);
	Report report{};
	check(client.decodeResult(report).status == ::telemetry::client::rpc::RpcClientStatus::Ok &&
	      report.total == 7);
	check(client.beginWriteField(total, std::uint32_t{42}) ==
	      ::telemetry::client::rpc::RpcClientStatus::Ok);
	complete();
	check(client.beginReadField(total) == ::telemetry::client::rpc::RpcClientStatus::Ok);
	complete();
	std::uint32_t value = 0;
	check(client.decodeResult(value).status == ::telemetry::client::rpc::RpcClientStatus::Ok &&
	      value == 42);
	float sameSize = 123;
	check(client.decodeResult(sameSize).status ==
	          ::telemetry::client::rpc::RpcClientStatus::WrongType &&
	      sameSize == 123);
	check(client.beginCommand(clear) == ::telemetry::client::rpc::RpcClientStatus::Ok);
	complete();
	check(device.total == 0);
	std::printf("rpc_client checks=%u failures=%u\n", checks, failures);
	return failures != 0;
}

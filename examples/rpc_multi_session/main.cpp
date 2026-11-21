/*
 * @file main.cpp
 * @brief Four typed RPC channels with static, independently owned mailboxes.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Fake complete-frame handoffs carry connection generation and channel metadata
 * outside ordinary Resource packets. Two independent connections and two
 * channels on a third connection share one serialized Model/Workspace owner.
 */
#include <client/rpc/RpcClient.hpp>
#include <integration/telemetry/v3/RpcSessionPool.hpp>
#include <telemetry/model/Model.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#ifndef RPC_SESSIONS_ARM_PROBE
#include <cstdio>
#endif

namespace example {
namespace ts = ::telemetry;
namespace rpc = resource::telemetry::v3;

struct Add {
	std::uint32_t amount;
};

struct Report {
	std::uint32_t total;
};

struct Counter {
	std::uint32_t total = 0;
	unsigned calls = 0;

	Report add(const Add& request) noexcept
	{
		++calls;
		total += request.amount;
		return {total};
	}
};

Counter counter;
inline constexpr auto branch = ts::group("counter", ts::service<&Counter::add>("Add", counter));
inline constexpr ts::Model model{branch};
inline constexpr auto view = model.view();
// The Service's ID from its path: group 0, entry 0.
inline constexpr ::telemetry::client::rpc::ServiceEndpoint<Add, Report> AddService{
    ts::idOf<model, "counter/Add">(), ::telemetry::client::rpc::trustedRpcMapping};
static_assert(ts::idOf<model, "counter/Add">() == 0x00000000u);
std::array<std::byte, model.maxScratch()> scratch{};
ts::Workspace workspace{scratch};

struct MailboxSlot {
	std::array<std::byte, 28> request{};
	std::array<std::byte, 4> result{};
	rpc::RpcMailbox mailbox{view, workspace, request, result};
	const resource::FileSystem<1> files{
	    resource::filesystem(resource::file("/telemetry/rpc.bin", mailbox))};
};

std::array<MailboxSlot, 4> slots{};
rpc::RpcSessionPool<4> sessions{
    std::array{rpc::RpcSessionBinding{slots[0].mailbox, slots[0].files.view()},
               rpc::RpcSessionBinding{slots[1].mailbox, slots[1].files.view()},
               rpc::RpcSessionBinding{slots[2].mailbox, slots[2].files.view()},
               rpc::RpcSessionBinding{slots[3].mailbox, slots[3].files.view()}}};

struct Channel {
	std::array<std::byte, 28> request{};
	std::array<std::byte, 4> result{};
	std::array<std::byte, 64> outgoing{};
	::telemetry::client::rpc::RpcClient client{
	    0, request, result, outgoing, ::telemetry::client::rpc::RpcClientOptions{4, 200, 1, 3}};
	rpc::RpcSessionHandle session{};
	std::uint32_t connection = 0;
	std::uint32_t channel = 0;
};

std::array<Channel, 4> channels{};

// These fields belong to framing and trusted connection context. A bare
// Resource reply has no request ID or channel: the application pairs it here.
struct FrameIdentity {
	std::uint32_t connection;
	std::uint32_t generation;
	std::uint32_t channel;
	std::uint32_t requestId;
};

struct Connection {
	std::uint32_t id;
	std::uint32_t generation = 1;
	bool occupied = false;
	FrameIdentity outstanding{};
};

std::array<Connection, 3> connections{{{101}, {202}, {303}}};
std::array<std::byte, 128> reply{};
std::array<std::byte, 64> delayedOldPacket{};
std::size_t delayedOldSize = 0;
rpc::RpcReplyTicket oldTicket{};
unsigned checks = 0;
std::uint32_t nowMs = 0;
bool loseFirstAck = true;
unsigned droppedAcks = 0;

void check(bool condition) noexcept
{
	++checks;
	if (!condition)
		std::abort();
}

void exchange(Channel& channel, Connection& connection) noexcept
{
	const auto packet = channel.client.nextPacket();
	if (!packet)
		return;
	// This example serializes exchanges on each physical connection. A real
	// multiplexer can pipeline only with equally explicit outer correlation.
	check(!connection.occupied);
	connection.occupied = true;
	connection.outstanding = {connection.id, packet.generation, channel.channel, packet.requestId};
	check(connection.outstanding.connection == channel.session.connection.peer);
	check(connection.outstanding.generation == connection.generation);
	check(connection.outstanding.channel == channel.session.connection.channel);
	check(channel.client.onSendAccepted(packet.generation, packet.requestId));
	const auto calls = counter.calls;
	const auto handled = sessions.handle(channel.session, packet.bytes, reply);
	check(handled.status == rpc::RpcSessionStatus::Ok);
	check(handled.reply.status == resource::Status::Ok && handled.reply.written != 0);
	check(counter.calls == calls); // Receiving a packet never invokes the Service.
	// sendReply has now copied these complete bytes to transport-owned storage.
	// The pool needs this exact final WRITE ticket before it may dispatch.
	if (handled.ticket.serial != 0)
		check(sessions.replyAccepted(channel.session, handled.ticket) == rpc::RpcSessionStatus::Ok);
	const bool drop = loseFirstAck && connection.id == 101 &&
	                  packet.bytes[0] == std::byte(resource::protocol::Op::Write);
	if (drop) {
		loseFirstAck = false;
		++droppedAcks;
		oldTicket = handled.ticket;
		delayedOldSize = packet.bytes.size();
		check(delayedOldSize <= delayedOldPacket.size());
		std::copy(packet.bytes.begin(), packet.bytes.end(), delayedOldPacket.begin());
	} else {
		// Return exactly the metadata paired when the complete frame was sent.
		check(channel.client.onReply(resource::Input{reply}.first(handled.reply.written),
		                             connection.outstanding.generation,
		                             connection.outstanding.requestId));
	}
	connection.occupied = false;
}

void complete() noexcept
{
	for (unsigned step = 0; step < 300; ++step, ++nowMs) {
		check(sessions.process(nowMs, 0).dispatched == 0);
		bool active = false;
		for (auto& channel : channels) {
			channel.client.process(nowMs);
			active = active || channel.client.active();
			exchange(channel, connections[channel.connection]);
		}
		const auto dispatched = sessions.process(nowMs); // Default: at most one callback.
		check(dispatched.status == rpc::RpcSessionStatus::Ok && dispatched.dispatched <= 1);
		if (!active)
			break;
	}
	for (auto& channel : channels)
		check(channel.client.completed());
}

void resultIs(Channel& channel, std::uint32_t expected) noexcept
{
	check(channel.client.diagnostic().dispatch == ts::DispatchStatus::Ok);
	check(channel.client.diagnostic().endpointStatus ==
	      static_cast<unsigned>(ts::ServiceStatus::Ok));
	Report result{};
	check(channel.client.decodeResult(result).status ==
	      ::telemetry::client::rpc::RpcClientStatus::Ok);
	check(result.total == expected);
}
} // namespace example

int main()
{
	using namespace example;
	constexpr std::array<std::uint32_t, 4> Amounts{7, 11, 13, 17};
	for (std::size_t i = 0; i < channels.size(); ++i) {
		auto& channel = channels[i];
		channel.connection = i < 3 ? static_cast<std::uint32_t>(i) : 2;
		channel.channel = i == 3 ? 1 : 0;
		const auto& connection = connections[channel.connection];
		const auto acquired =
		    sessions.acquire({connection.id, connection.generation, channel.channel});
		check(acquired.status == rpc::RpcSessionStatus::Ok);
		channel.session = acquired.session;
		check(channel.client.startSession(connection.generation) ==
		      ::telemetry::client::rpc::RpcClientStatus::Ok);
		check(channel.client.beginService(AddService, Add{Amounts[i]}) ==
		      ::telemetry::client::rpc::RpcClientStatus::Ok);
	}
	check(sessions.acquire({404, 1, 0}).status == rpc::RpcSessionStatus::NoCapacity);
	complete();
	check(droppedAcks == 1 && counter.calls == 4 && counter.total == 48);
	constexpr std::array<std::uint32_t, 4> Totals{7, 18, 31, 48};
	for (std::size_t i = 0; i < channels.size(); ++i)
		resultIs(channels[i], Totals[i]);
	const auto previous = channels[0].session;
	check(sessions.release(previous) == rpc::RpcSessionStatus::Ok);
	check(!sessions.valid(previous) &&
	      slots[0].mailbox.state() == ::telemetry::protocol::rpc::RpcState::Idle);
	channels[0].client.loseSession(1);
	connections[0].generation = 2; // Admission excludes generation 1 before reuse.
	const auto reacquired = sessions.acquire({101, 2, 0});
	check(reacquired.status == rpc::RpcSessionStatus::Ok &&
	      reacquired.session.index == previous.index);
	check(reacquired.session.generation != previous.generation);
	channels[0].session = reacquired.session;
	check(sessions.handle(previous, resource::Input{delayedOldPacket}.first(delayedOldSize), reply)
	          .status == rpc::RpcSessionStatus::InvalidSession);
	check(sessions.replyAccepted(reacquired.session, oldTicket) ==
	      rpc::RpcSessionStatus::StaleReply);
	check(channels[0].client.startSession(2) == ::telemetry::client::rpc::RpcClientStatus::Ok);
	check(channels[0].client.beginService(AddService, Add{5}) ==
	      ::telemetry::client::rpc::RpcClientStatus::Ok);
	complete();
	check(counter.calls == 5 && counter.total == 53);
	resultIs(channels[0], 53);
	for (std::size_t i = 1; i < channels.size(); ++i) {
		resultIs(channels[i], Totals[i]);
		check(sessions.valid(channels[i].session));
		check(slots[i].mailbox.state() == ::telemetry::protocol::rpc::RpcState::Ready);
	}
#ifndef RPC_SESSIONS_ARM_PROBE
	std::printf("RPC sessions example: %u checks, four independent channels, "
	            "one lost ACK, safe reuse, five callbacks\n",
	            checks);
#endif
	return 0;
}

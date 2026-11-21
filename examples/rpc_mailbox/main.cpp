/*
 * @file main.cpp
 * @brief Host RPC mailbox integration with external connection generations.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 *
 * One owner consumes complete Resource packets, hands off each ACK before
 * dispatch, and rejects packets from an earlier connection before the provider.
 */

#include <resource/FileSystem.hpp>
#include <resource/protocol/Packets.hpp>
#include <resource/protocol/Protocol.hpp>
#include <integration/telemetry/v3/RpcMailbox.hpp>
#include <telemetry/Telemetry.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#ifndef RPC_ARM_PROBE
#include <cstdio>
#endif

namespace example {
namespace ts = ::telemetry;
namespace rs = resource::telemetry::v3;
namespace client = resource::protocol::client;

unsigned checks = 0;

void check(bool condition) noexcept
{
	++checks;
	if (!condition) {
#ifndef RPC_ARM_PROBE
		std::fprintf(stderr, "RPC mailbox example failed at check %u\n", checks);
#endif
		std::abort();
	}
}

struct Add {
	std::uint32_t amount;
};

struct Report {
	std::uint32_t value;
};

struct Counter {
	std::uint32_t value = 0;
	unsigned calls = 0;

	ts::ServiceResult<Report> add(const Add& request) noexcept
	{
		++calls;
		value += request.amount;
		return ts::ServiceResult<Report>::success(Report{value});
	}
};

// All borrowed objects keep stable addresses for the complete owner lifetime.
Counter counter;
inline constexpr auto branch = ts::group("counter", ts::service<&Counter::add>("Add", counter));
inline constexpr ts::Model model{branch};
inline constexpr auto modelView = model.view();
// The ID a TRPC request carries, taken from the Service's path.
inline constexpr auto AddId = ts::idOf<model, "counter/Add">();
static_assert(AddId == ts::makeId<0, 0>()); // The positional form, being removed.
std::array<std::byte, 64> scratch{};
ts::Workspace workspace{scratch};
std::array<std::byte, 64> requestStorage{};
std::array<std::byte, 16> resultStorage{};
rs::RpcMailbox mailbox{modelView, workspace, requestStorage, resultStorage};
const auto files = resource::filesystem(resource::file("/telemetry/rpc.bin", mailbox));
inline constexpr resource::FileIndex RpcFile = 0;

// These helpers describe the RPC wire bytes, never the native struct layout.
void put32(resource::Output bytes, std::size_t offset, std::uint32_t value) noexcept
{
	for (unsigned i = 0; i < 4; ++i)
		bytes[offset + i] = std::byte((value >> (8 * i)) & 0xffu);
}

std::uint32_t get32(resource::Input bytes, std::size_t offset) noexcept
{
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
	return value;
}

using Request =
    std::array<std::byte, ::telemetry::protocol::rpc::rpcRequestHeaderBytes + ts::wireSize<Add>>;

Request makeRequest(std::uint32_t transactionId, const Add& request) noexcept
{
	Request bytes{};
	bytes[0] = std::byte{'T'};
	bytes[1] = std::byte{'R'};
	bytes[2] = std::byte{'P'};
	bytes[3] = std::byte{'C'};
	bytes[4] = std::byte{1}; // u16 major=1; minor, flags and reserved remain zero.
	put32(bytes, 8, transactionId);
	put32(bytes, 12, AddId);
	put32(bytes, 16, ts::wireSize<Add>);
	bytes[20] = std::byte(::telemetry::protocol::rpc::RpcOperation::Service);
	check(ts::encode(request, resource::Output{bytes}.subspan(
	                              ::telemetry::protocol::rpc::rpcRequestHeaderBytes)) ==
	      ts::CodecStatus::Ok);
	return bytes;
}

// Ingress stamps metadata from its connection context when a frame completes.
// connectionGeneration is local queue metadata, absent from every wire packet.
struct Packet {
	std::uint32_t peer = 0;
	std::uint32_t connectionGeneration = 0;
	std::array<std::byte, 128> bytes{};
	std::size_t size = 0;
};

Packet packet(std::uint32_t peer, std::uint32_t generation, resource::Input bytes) noexcept
{
	check(bytes.size() <= Packet{}.bytes.size());
	Packet result{peer, generation};
	std::copy(bytes.begin(), bytes.end(), result.bytes.begin());
	result.size = bytes.size();
	return result;
}

// This host-only queue receives already framed packets, not TCP/UART chunks.
class Queue {
public:
	void push(const Packet& packet) noexcept
	{
		check(count_ < packets_.size());
		packets_[(head_ + count_) % packets_.size()] = packet;
		++count_;
	}

	bool pop(Packet& packet) noexcept
	{
		if (count_ == 0)
			return false;
		packet = packets_[head_];
		head_ = (head_ + 1) % packets_.size();
		--count_;
		return true;
	}

private:
	std::array<Packet, 4> packets_{};
	std::size_t head_ = 0;
	std::size_t count_ = 0;
};

// send() copies a reply into transport-owned storage before returning.
struct HostTransport {
	std::array<std::byte, 128> reply{};
	std::size_t size = 0;
	unsigned sent = 0;

	void send(resource::Input bytes) noexcept
	{
		check(bytes.size() <= reply.size());
		std::copy(bytes.begin(), bytes.end(), reply.begin());
		size = bytes.size();
		++sent;
	}

	resource::Input lastReply() const noexcept
	{
		return resource::Input{reply}.first(size);
	}
};

// Public methods: generation() supplies ingress metadata; disconnect()/admit()
// change admission; step() serializes clock, packet operations and dispatch.
class Owner {
public:
	Owner(Queue& queue, HostTransport& transport, std::uint32_t peer) noexcept
	    : queue_(queue), transport_(transport), peer_(peer)
	{}

	std::uint32_t generation() const noexcept
	{
		return generation_;
	}

	void disconnect() noexcept
	{
		admitted_ = false;
		++generation_;          // Production code must prevent reuse while old frames exist.
		mailbox.resetSession(); // Called by this owner, outside an active callback.
	}

	void admit(std::uint32_t peer) noexcept
	{
		// Descriptor fingerprint agreement for this connection precedes admission.
		peer_ = peer;
		admitted_ = true;
	}

	void step(std::uint32_t nowMs) noexcept
	{
		// A Pending request here had its final ACK handed off on an earlier step.
		const bool dispatched = mailbox.state() == ::telemetry::protocol::rpc::RpcState::Pending;
		mailbox.process(nowMs);
		// A callback can take time. Return to the application's clock before
		// accepting another WRITE rather than stamping it with this old nowMs.
		if (dispatched)
			return;
		Packet next;
		if (!queue_.pop(next))
			return;
		if (!admitted_ || next.peer != peer_ || next.connectionGeneration != generation_) {
			++rejected;
			return; // Never let an old connection reach Resource or the mailbox.
		}
		const auto reply = resource::protocol::process(
		    files.view(), resource::Input{next.bytes}.first(next.size), response_);
		if (reply.written != 0)
			transport_.send(resource::Input{response_}.first(reply.written));
		// No process() here: the final WRITE ACK leaves before callback dispatch.
	}

	unsigned rejected = 0;

private:
	Queue& queue_;
	HostTransport& transport_;
	std::uint32_t peer_;
	std::uint32_t generation_ = 1;
	bool admitted_ = true;
	std::array<std::byte, 128> response_{};
};
} // namespace example

int main()
{
	using namespace example;
	constexpr std::uint32_t Peer = 42;
	constexpr std::uint32_t Tx = 1;
	Queue queue;
	HostTransport transport;
	Owner owner{queue, transport, Peer};
	const auto request = makeRequest(Tx, Add{7});
	std::array<std::byte, 128> writeBytes{};
	const auto write = client::makeWrite(writeBytes, RpcFile, 0, request, true);
	check(write.status == client::BuildStatus::Ok);
	const auto writePacket = resource::Input{writeBytes}.first(write.written);

	const auto delayedOldPacket = packet(Peer, owner.generation(), writePacket);
	owner.disconnect();
	owner.admit(Peer);            // Same peer and tx are valid only in this new generation.
	queue.push(delayedOldPacket); // Old ingress can enqueue even after a queue clear.
	queue.push(packet(Peer + 1, owner.generation(), writePacket));
	const auto currentPacket = packet(Peer, owner.generation(), writePacket);
	queue.push(currentPacket);
	owner.step(10);
	owner.step(11);
	check(owner.rejected == 2 && counter.calls == 0 && transport.sent == 0);
	owner.step(12);
	const auto ack = client::parseWrite(transport.lastReply(), request.size());
	check(ack && !ack.shortError && ack.response.status == resource::Status::Ok);
	check(ack.response.consumed == request.size() && ack.response.complete);
	check(ack.response.next == ::telemetry::protocol::rpc::rpcCursor(Tx, request.size()));
	check(mailbox.state() == ::telemetry::protocol::rpc::RpcState::Pending && counter.calls == 0);

	owner.step(13); // Only now may the Service run.
	check(mailbox.state() == ::telemetry::protocol::rpc::RpcState::Ready && counter.calls == 1);
	queue.push(currentPacket); // Lost ACK retry in the same admitted session.
	owner.step(14);
	const auto repeatedAck = client::parseWrite(transport.lastReply(), request.size());
	check(repeatedAck && repeatedAck.response.status == resource::Status::Ok &&
	      repeatedAck.response.complete && counter.calls == 1);

	std::array<std::byte, 32> readBytes{};
	const auto read = client::makeRead(readBytes, RpcFile, 0); // Not WRITE's next.
	check(read.status == client::BuildStatus::Ok);
	queue.push(packet(Peer, owner.generation(), resource::Input{readBytes}.first(read.written)));
	owner.step(15);
	const auto parsed = client::parseRead(transport.lastReply());
	check(parsed && !parsed.shortError && parsed.response.status == resource::Status::Ok);
	const auto record = parsed.response.data; // Borrowed until transport reply changes.
	check(parsed.response.eof &&
	      record.size() == ::telemetry::protocol::rpc::rpcReplyHeaderBytes + 4);
	check(record[0] == std::byte{'T'} && record[1] == std::byte{'R'} &&
	      record[2] == std::byte{'P'} && record[3] == std::byte{'S'});
	check(record[4] == std::byte{1} && record[5] == std::byte{} && record[6] == std::byte{} &&
	      record[7] == std::byte{});
	check(get32(record, 8) == Tx && get32(record, 12) == AddId && get32(record, 16) == 4);
	check(record[20] == std::byte(::telemetry::protocol::rpc::RpcOperation::Service));
	check(record[21] ==
	      std::byte(::telemetry::protocol::rpc::RpcState::Ready)); // Check state before statuses.
	check(record[22] == std::byte(ts::DispatchStatus::Ok) &&
	      record[23] == std::byte(ts::ServiceStatus::Ok));
	check(get32(record, 24) == request.size() && get32(record, 28) == request.size());
	Report result{};
	check(ts::decode(record.subspan(::telemetry::protocol::rpc::rpcReplyHeaderBytes), result) ==
	      ts::CodecStatus::Ok);
	check(result.value == 7 && counter.value == 7 && counter.calls == 1);
	owner.disconnect();
	check(mailbox.state() == ::telemetry::protocol::rpc::RpcState::Idle);
#ifndef RPC_ARM_PROBE
	std::printf("RPC mailbox example: %u checks, stale/foreign packets rejected, "
	            "one callback, result=%u\n",
	            checks, unsigned(result.value));
#endif
	return 0;
}

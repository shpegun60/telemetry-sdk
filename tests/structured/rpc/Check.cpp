/* RpcMailbox contract: deterministic framing, replay, state and checked dispatch. MIT. */
#include "Fixture.hpp"
#include <resource/FileSystem.hpp>
#include <resource/protocol/Protocol.hpp>
#include <cstdio>
#include <limits>

using namespace rpc_fixture;
static unsigned checks = 0, failures = 0;

static void check(bool condition, unsigned line) noexcept
{
	++checks;
	if (!condition) {
		++failures;
		std::fprintf(stderr, "rpc Check.cpp:%u failed (%u)\n", line, checks);
	}
}

#define CHECK(condition) check(bool(condition), __LINE__)

static void writeIs(const resource::WriteResult& result, R status, resource::Cursor next,
                    std::uint32_t consumed = 0, bool complete = false) noexcept
{
	CHECK(result.status == status);
	CHECK(result.next == next);
	CHECK(result.consumed == consumed);
	CHECK(result.complete == complete);
}

static void readError(rpc::RpcMailbox& mailbox, resource::Cursor at, R status) noexcept
{
	Guarded<40> out;
	const auto before = out.bytes;
	const auto result = mailbox.read(at, out.output());
	CHECK(result.status == status);
	CHECK(result.next == at);
	CHECK(result.written == 0 && !result.eof);
	CHECK(out.bytes == before);
}

static auto snapshot(rpc::RpcMailbox& mailbox, State state, std::uint32_t tx,
                     std::uint32_t payload = 0, unsigned dispatch = 0,
                     unsigned endpoint = 0) noexcept
{
	std::array<std::byte, 32> out{};
	const auto read = mailbox.read(0, out);
	CHECK(read.status == R::Ok && read.written == 32);
	CHECK(get32(out, 0) == 0x53505254u && get32(out, 4) == 1);
	CHECK(get32(out, 8) == tx && get32(out, 16) == payload);
	CHECK(out[21] == std::byte(static_cast<unsigned>(state)));
	CHECK(out[22] == std::byte(dispatch) && out[23] == std::byte(endpoint));
	CHECK(read.eof == (state != State::Ready || payload == 0));
	CHECK(read.next == (state == State::Ready ? cursor(tx, 32) : 0));
	CHECK(mailbox.size() == 32 + payload);
	CHECK(mailbox.state() == state);
	return out;
}

static void start(rpc::RpcMailbox& mailbox, Input bytes, std::uint32_t tx) noexcept
{
	writeIs(mailbox.write(0, bytes, true), R::Ok,
	        cursor(tx, static_cast<std::uint32_t>(bytes.size())),
	        static_cast<std::uint32_t>(bytes.size()), true);
}

static void resetDevice() noexcept
{
	device = Device{};
	slot.reset();
}

static void wireAndRead() noexcept
{
	resetDevice();
	Harness<> h;
	CHECK(::telemetry::protocol::rpc::rpcRequestHeaderBytes == 24 &&
	      ::telemetry::protocol::rpc::rpcReplyHeaderBytes == 32);
	CHECK(::telemetry::protocol::rpc::rpcReceiveTimeoutMs == 10000);
	const auto idle = snapshot(h.mailbox, State::Idle, 0);
	CHECK(get32(idle, 12) == 0 && get32(idle, 24) == 0 && get32(idle, 28) == 0);
	Golden value{};
	const auto request = packet(Op::Command, 0x00010002, value, 125);
	const std::array<unsigned char, 24> requestGolden{0x54, 0x52, 0x50, 0x43, 0x01, 0, 0,    0,
	                                                  0x7d, 0,    0,    0,    0x02, 0, 0x01, 0,
	                                                  0x60, 0,    0,    0,    0x02, 0, 0,    0};
	for (unsigned i = 0; i < requestGolden.size(); ++i)
		CHECK(request[i] == std::byte(requestGolden[i]));
	start(h.mailbox, request, 125);
	CHECK(callbacks() == 0);
	const auto pending = snapshot(h.mailbox, State::Pending, 125);
	CHECK(get32(pending, 24) == 120 && get32(pending, 28) == 120);
	readError(h.mailbox, cursor(125, 32), R::InvalidCursor);
	h.mailbox.process(1);
	const auto out = snapshot(h.mailbox, State::Ready, 125);
	const std::array<unsigned char, 32> responseGolden{
	    0x54, 0x52, 0x50, 0x53, 0x01, 0,    0, 0, 0x7d, 0, 0, 0, 0x02, 0, 0x01, 0,
	    0,    0,    0,    0,    0x02, 0x04, 0, 0, 0x78, 0, 0, 0, 0x78, 0, 0,    0};
	for (unsigned i = 0; i < responseGolden.size(); ++i)
		CHECK(out[i] == std::byte(responseGolden[i]));
	CHECK(callbacks() == 1);
	h.mailbox.process(2);
	CHECK(callbacks() == 1);
	writeIs(h.mailbox.write(0, request, true), R::Ok, cursor(125, 120), 120, true);
	CHECK(snapshot(h.mailbox, State::Ready, 125) == out && callbacks() == 1);
	readError(h.mailbox, cursor(125, 31), R::InvalidCursor);
	readError(h.mailbox, cursor(125, 33), R::InvalidCursor);
	readError(h.mailbox, cursor(124, 32), R::CursorExpired);
	readError(h.mailbox, cursor(126, 32), R::InvalidCursor);
	readError(h.mailbox, 32, R::InvalidCursor);
	std::array<std::byte, 0> empty{};
	const auto eof = h.mailbox.read(cursor(125, 32), empty);
	CHECK(eof.status == R::Ok && eof.eof && eof.written == 0 && eof.next == cursor(125, 32));
	for (unsigned capacity = 0; capacity < 32; ++capacity) {
		Guarded<40> small;
		const auto before = small.bytes;
		const auto r = h.mailbox.read(0, small.output().first(capacity));
		CHECK(r.status == R::BufferTooSmall && r.next == 0 && r.written == 0 && !r.eof);
		CHECK(small.bytes == before);
	}
	CHECK(h.intact());

	h.mailbox.resetSession();
	const Config config{0x12345678, true};
	const auto echo = packet(Op::Service, 0, config);
	start(h.mailbox, echo, 1);
	h.mailbox.process(3);
	const auto ready = snapshot(h.mailbox, State::Ready, 1, 5);
	CHECK(get32(ready, 12) == 0 && ready[20] == std::byte{3});
	for (unsigned first = 32; first <= 40; ++first) {
		Guarded<40> output;
		const auto r = h.mailbox.read(0, output.output().first(first));
		const unsigned sent = std::min(5u, first - 32);
		CHECK(r.written == 32 + sent && r.next == cursor(1, 32 + sent));
		CHECK(r.eof == (sent == 5));
		CHECK(output.intact());
		for (unsigned i = 0; i < sent; ++i)
			CHECK(output.output()[32 + i] == echo[24 + i]);
	}
	for (unsigned offset = 32; offset <= 37; ++offset) {
		for (unsigned capacity = 0; capacity <= 7; ++capacity) {
			Guarded<8> output;
			const auto before = output.bytes;
			const auto r = h.mailbox.read(cursor(1, offset), output.output().first(capacity));
			const unsigned sent = std::min(37 - offset, capacity);
			if (capacity == 0 && offset != 37) {
				CHECK(r.status == R::BufferTooSmall && r.written == 0 && !r.eof);
				CHECK(r.next == cursor(1, offset) && output.bytes == before);
			} else {
				CHECK(r.status == R::Ok && r.written == sent && r.next == cursor(1, offset + sent));
				CHECK(r.eof == (offset + sent == 37));
				for (unsigned i = 0; i < sent; ++i)
					CHECK(output.output()[i] == echo[24 + offset - 32 + i]);
			}
			CHECK(output.intact());
		}
	}
	CHECK(callbacks() == 2 && h.intact());
}

static void fragmentationAndFinal() noexcept
{
	const auto request = packet(Op::FieldWrite, 0, Config{91, false}, 10);
	for (unsigned first = 24; first <= request.size(); ++first) {
		for (unsigned second = first; second <= request.size(); ++second) {
			resetDevice();
			Harness<> h;
			h.mailbox.process(20);
			writeIs(h.mailbox.write(0, Input{request}.first(first), false), R::Ok,
			        cursor(10, first), first);
			if (second > first)
				writeIs(h.mailbox.write(cursor(10, first),
				                        Input{request}.subspan(first, second - first), false),
				        R::Ok, cursor(10, second), second - first);
			const auto complete =
			    h.mailbox.write(cursor(10, second), Input{request}.subspan(second), true);
			writeIs(complete, R::Ok, cursor(10, 29), 29 - second, true);
			CHECK(callbacks() == 0);
			snapshot(h.mailbox, State::Pending, 10);
			writeIs(h.mailbox.write(0, Input{request}.first(first), false), R::Ok,
			        cursor(10, first), first);
			writeIs(h.mailbox.write(cursor(10, second), Input{request}.subspan(second), true),
			        R::Ok, cursor(10, 29), 29 - second, true);
			h.mailbox.process(21);
			CHECK(callbacks() == 1 && device.current.value == 91);
			writeIs(h.mailbox.write(0, request, true), R::Ok, cursor(10, 29), 29, true);
			h.mailbox.process(22);
			CHECK(callbacks() == 1 && h.intact());
		}
	}
	resetDevice();
	Harness<> h;
	writeIs(h.mailbox.write(0, Input{request}.first(24), false), R::Ok, cursor(10, 24), 24);
	const auto saved = h.request.bytes;
	writeIs(h.mailbox.write(cursor(10, 24), Input{request}.subspan(24, 1), true), R::InvalidData,
	        cursor(10, 24));
	writeIs(h.mailbox.write(cursor(10, 25), Input{request}.subspan(25, 1), false), R::InvalidCursor,
	        cursor(10, 25));
	writeIs(h.mailbox.write(cursor(10, 23), Input{request}.subspan(23, 2), false), R::InvalidCursor,
	        cursor(10, 23));
	writeIs(h.mailbox.write(cursor(10, 24), {}, false), R::InvalidData, cursor(10, 24));
	writeIs(h.mailbox.write(cursor(11, 24), Input{request}.subspan(24), true), R::InvalidCursor,
	        cursor(11, 24));
	writeIs(h.mailbox.write(24, Input{request}.subspan(24), true), R::InvalidCursor, 24);
	writeIs(h.mailbox.write(cursor(9, 24), Input{request}.subspan(24), true), R::CursorExpired,
	        cursor(9, 24));
	CHECK(h.request.bytes == saved && callbacks() == 0);
	auto changed = request;
	changed[12] ^= std::byte{1};
	writeIs(h.mailbox.write(0, Input{changed}.first(24), false), R::InvalidData, 0);
	writeIs(h.mailbox.write(cursor(10, 28), Input{request}.subspan(27, 2), false), R::InvalidData,
	        cursor(10, 28));
	writeIs(h.mailbox.write(cursor(10, 24), Input{request}.subspan(24), false), R::Ok,
	        cursor(10, 29), 5);
	snapshot(h.mailbox, State::Receiving, 10);
	writeIs(h.mailbox.write(cursor(10, 29), {}, true), R::Ok, cursor(10, 29), 0, true);
	writeIs(h.mailbox.write(cursor(10, 29), {}, true), R::Ok, cursor(10, 29), 0, true);
	h.mailbox.process(1);
	CHECK(callbacks() == 1);
	writeIs(h.mailbox.write(cursor(10, 29), {}, true), R::Ok, cursor(10, 29), 0, true);
	changed = request;
	changed.back() ^= std::byte{1};
	writeIs(h.mailbox.write(0, changed, true), R::InvalidData, 0);
	CHECK(callbacks() == 1 && h.intact());
}

static void rejectedStarts() noexcept
{
	resetDevice();
	Harness<64, 16> h;
	const auto old = packet(Op::Service, 0, Config{77, true}, 4);
	start(h.mailbox, old, 4);
	h.mailbox.process(0);
	std::array<std::byte, 64> before{};
	const auto oldRead = h.mailbox.read(0, before);
	const auto savedRequest = h.request.bytes;
	const auto savedResult = h.result.bytes;
	const auto fresh = packet(Op::FieldWrite, 0, Config{88, false}, 5);
	for (unsigned length = 0; length < 24; ++length)
		writeIs(h.mailbox.write(0, Input{fresh}.first(length), false), R::InvalidData, 0);
	for (unsigned offset : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 20u, 21u, 22u, 23u}) {
		auto bad = fresh;
		bad[offset] = std::byte{0xff};
		writeIs(h.mailbox.write(0, bad, true), R::InvalidData, 0);
	}
	for (std::uint32_t length : {UINT32_MAX, UINT32_MAX - 23}) {
		auto bad = fresh;
		put(bad, 16, length);
		writeIs(h.mailbox.write(0, bad, false), R::InvalidData, 0);
	}
	auto tooLarge = fresh;
	put(tooLarge, 16, std::uint32_t{41});
	writeIs(h.mailbox.write(0, tooLarge, false), R::BufferTooSmall, 0);
	auto zero = fresh;
	put(zero, 8, std::uint32_t{0});
	writeIs(h.mailbox.write(0, zero, true), R::InvalidData, 0);
	auto readPayload = fresh;
	readPayload[20] = std::byte{4};
	writeIs(h.mailbox.write(0, readPayload, true), R::InvalidData, 0);
	writeIs(h.mailbox.write(0, Input{fresh}.first(24), true), R::InvalidData, 0);
	std::array<std::byte, 30> extra{};
	std::copy(fresh.begin(), fresh.end(), extra.begin());
	writeIs(h.mailbox.write(0, extra, true), R::InvalidData, 0);
	CHECK(h.request.bytes == savedRequest && h.result.bytes == savedResult);
	std::array<std::byte, 64> after{};
	const auto newRead = h.mailbox.read(0, after);
	CHECK(before == after && oldRead.next == newRead.next && callbacks() == 1);
	start(h.mailbox, fresh, 5);
	h.mailbox.process(1);
	CHECK(callbacks() == 2 && device.current.value == 88 && h.intact());
	readError(h.mailbox, cursor(4, 32), R::CursorExpired);
}

static void timeAndSession() noexcept
{
	resetDevice();
	Harness<> h;
	const auto request = packet(Op::Command, 0, Config{99, true}, 126);
	h.mailbox.process(1000);
	writeIs(h.mailbox.write(0, Input{request}.first(24), false), R::Ok, cursor(126, 24), 24);
	h.mailbox.process(10999);
	snapshot(h.mailbox, State::Receiving, 126);
	writeIs(h.mailbox.write(0, Input{request}.first(24), false), R::Ok, cursor(126, 24), 24);
	writeIs(h.mailbox.write(cursor(126, 24), Input{request}.subspan(24, 1), true), R::InvalidData,
	        cursor(126, 24));
	snapshot(h.mailbox, State::Receiving, 126); // READ and refused input do not refresh the timer.
	h.mailbox.process(11000);
	snapshot(h.mailbox, State::Idle, 126);
	writeIs(h.mailbox.write(0, request, true), R::CursorExpired, 0);
	readError(h.mailbox, cursor(126, 32), R::CursorExpired);
	auto next = request;
	put(next, 8, std::uint32_t{127});
	start(h.mailbox, next, 127);
	snapshot(h.mailbox, State::Pending, 127);
	auto newer = request;
	put(newer, 8, std::uint32_t{128});
	writeIs(h.mailbox.write(0, newer, true), R::InvalidData, 0);
	h.mailbox.resetSession();
	CHECK(callbacks() == 0);
	snapshot(h.mailbox, State::Idle, 0);
	start(h.mailbox, request, 126);
	h.mailbox.process(11001);
	CHECK(callbacks() == 1);
	h.mailbox.process(1000000);
	snapshot(h.mailbox, State::Ready, 126);
	h.mailbox.resetSession();
	h.mailbox.process(UINT32_MAX - 4999);
	writeIs(h.mailbox.write(0, Input{request}.first(24), false), R::Ok, cursor(126, 24), 24);
	h.mailbox.process(4999);
	snapshot(h.mailbox, State::Receiving, 126);
	h.mailbox.process(5000);
	snapshot(h.mailbox, State::Idle, 126);
	h.mailbox.resetSession();
	h.mailbox.process(100);
	writeIs(h.mailbox.write(0, Input{request}.first(24), false), R::Ok, cursor(126, 24), 24);
	h.mailbox.process(9000);
	writeIs(h.mailbox.write(cursor(126, 24), Input{request}.subspan(24, 1), false), R::Ok,
	        cursor(126, 25), 1);
	h.mailbox.process(18999);
	snapshot(h.mailbox, State::Receiving, 126);
	h.mailbox.process(19000);
	snapshot(h.mailbox, State::Idle, 126);
	h.mailbox.resetSession();
	const auto max = emptyPacket(Op::Command, 1, UINT32_MAX);
	start(h.mailbox, max, UINT32_MAX);
	h.mailbox.process(19001);
	const auto one = emptyPacket(Op::Command, 1, 1);
	writeIs(h.mailbox.write(0, one, true), R::CursorExpired, 0);
	h.mailbox.resetSession();
	start(h.mailbox, one, 1);
	h.mailbox.process(19002);
	CHECK(callbacks() == 3 && h.intact());
	// A newer valid Receiving request cancels old bytes; it does not execute them.
	h.mailbox.resetSession();
	writeIs(h.mailbox.write(0, Input{request}.first(24), false), R::Ok, cursor(126, 24), 24);
	writeIs(h.mailbox.write(0, Input{next}.first(24), false), R::Ok, cursor(127, 24), 24);
	writeIs(h.mailbox.write(cursor(126, 24), Input{request}.subspan(24), true), R::CursorExpired,
	        cursor(126, 24));
	CHECK(callbacks() == 3);
	h.mailbox.resetSession();
	CHECK(h.intact());
}

static rpc::RpcMailbox* activeMailbox = nullptr;
static Input activeRequest;
static unsigned hookCalls = 0;

static void runningHook() noexcept
{
	++hookCalls;
	snapshot(*activeMailbox, State::Running, 55);
	writeIs(activeMailbox->write(0, activeRequest, true), R::Ok, cursor(55, 29), 29, true);
	writeIs(activeMailbox->write(cursor(55, 29), {}, true), R::Ok, cursor(55, 29), 0, true);
	const auto next = emptyPacket(Op::Command, 1, 56);
	writeIs(activeMailbox->write(0, next, true), R::InvalidData, 0);
	readError(*activeMailbox, cursor(55, 32), R::InvalidCursor);
}

static void reentrantRunning() noexcept
{
	resetDevice();
	Harness<> h;
	const auto request = packet(Op::Command, 0, Config{7, true}, 55);
	activeMailbox = &h.mailbox;
	activeRequest = request;
	hookCalls = 0;
	device.hook = runningHook;
	start(h.mailbox, request, 55);
	h.mailbox.process(0);
	device.hook = nullptr;
	activeMailbox = nullptr;
	activeRequest = {};
	CHECK(hookCalls == 1 && callbacks() == 1);
	snapshot(h.mailbox, State::Ready, 55);
	h.mailbox.process(1);
	CHECK(callbacks() == 1 && h.intact());
}

static void modelOperations() noexcept
{
	resetDevice();
	Harness<> h;
	std::uint32_t tx = 1;
	const Config value{0x87654321, true};
	for (unsigned status = 0; status < 6; ++status) {
		device.writeStatus = static_cast<W>(status);
		const auto p = packet(Op::FieldWrite, 0, value, tx);
		const auto before = callbacks();
		start(h.mailbox, p, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, 0, 0, status);
		CHECK(callbacks() == before + 1);
		++tx;
	}
	for (unsigned status = 0; status < 8; ++status) {
		device.commandStatus = static_cast<C>(status);
		const auto p = packet(Op::Command, 0, value, tx);
		const auto before = callbacks();
		start(h.mailbox, p, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, 0, 0, status);
		CHECK(callbacks() == before + 1);
		++tx;
	}
	for (unsigned status = 0; status < 5; ++status) {
		device.serviceStatus = static_cast<S>(status);
		const auto p = packet(Op::Service, 0, value, tx);
		const auto before = callbacks();
		start(h.mailbox, p, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, status == 0 ? 5 : 0, 0, status);
		CHECK(callbacks() == before + 1);
		++tx;
		const auto ping = emptyPacket(Op::Service, 1, tx);
		start(h.mailbox, ping, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, 0, 0, status);
		++tx;
	}
	device.writeStatus = W::Applied;
	device.commandStatus = C::Executed;
	device.serviceStatus = S::Ok;
	for (const Op op : {Op::FieldWrite, Op::Command, Op::Service}) {
		auto p = packet(op, 0, value, tx);
		p.back() = std::byte{2};
		const auto before = callbacks();
		start(h.mailbox, p, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, 0, 5);
		CHECK(callbacks() == before);
		++tx;
		p = packet(op, 2, value, tx);
		start(h.mailbox, p, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, 0, 9);
		CHECK(callbacks() == before);
		++tx;
		slot.bind(device);
		p = packet(op, 2, value, tx);
		start(h.mailbox, p, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, op == Op::Service ? 5 : 0);
		CHECK(callbacks() == before + 1);
		slot.reset();
		++tx;
	}
	for (const Op op : {Op::FieldRead, Op::FieldWrite, Op::Command, Op::Service}) {
		const auto p = emptyPacket(op, 0xffffffff, tx);
		const auto before = callbacks();
		start(h.mailbox, p, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, 0, 4);
		CHECK(callbacks() == before);
		++tx;
	}
	const auto ro = emptyPacket(Op::FieldWrite, 1, tx);
	const auto before = callbacks();
	start(h.mailbox, ro, tx);
	h.mailbox.process(tx);
	snapshot(h.mailbox, State::Ready, tx, 0, 0, 2);
	CHECK(callbacks() == before);
	++tx;
	for (const std::uint32_t id : {0u, 4u}) {
		const auto read = emptyPacket(Op::FieldRead, id, tx);
		start(h.mailbox, read, tx);
		h.mailbox.process(tx);
		snapshot(h.mailbox, State::Ready, tx, 5);
		std::array<std::byte, 37> out{};
		const auto r = h.mailbox.read(0, out);
		CHECK(r.eof && r.written == 37 && get32(out, 32) == value.value && out[36] == std::byte{1});
		++tx;
	}
	const auto command = emptyPacket(Op::Command, 1, tx);
	start(h.mailbox, command, tx);
	h.mailbox.process(tx);
	snapshot(h.mailbox, State::Ready, tx);
	++tx;
	device.commandStatus = static_cast<C>(255);
	const auto invalid = emptyPacket(Op::Command, 1, tx);
	const auto n = callbacks();
	start(h.mailbox, invalid, tx);
	h.mailbox.process(tx);
	snapshot(h.mailbox, State::Ready, tx, 0, 8);
	writeIs(h.mailbox.write(0, invalid, true), R::Ok, cursor(tx, 24), 24, true);
	h.mailbox.process(tx + 1);
	CHECK(callbacks() == n + 1 && h.workspace.used() == 0 && h.intact());
	++tx;
	device.writeStatus = static_cast<W>(255);
	const auto badWrite = packet(Op::FieldWrite, 0, value, tx);
	start(h.mailbox, badWrite, tx);
	h.mailbox.process(tx);
	snapshot(h.mailbox, State::Ready, tx, 0, 8);
	CHECK(h.intact());
}

static void packetLifetimesAndAlignment() noexcept
{
	for (unsigned alignment = 0; alignment < 8; ++alignment) {
		resetDevice();
		Harness<> h;
		const auto p = packet(Op::Service, 0, Config{0x10203040, true});
		Guarded<48> incoming;
		std::copy(p.begin(), p.end(), incoming.output().begin() + alignment);
		start(h.mailbox, Input{incoming.output()}.subspan(alignment, p.size()), 1);
		incoming.bytes.fill(std::byte{0xee}); // Packet ownership ends at WRITE return.
		h.mailbox.process(0);
		snapshot(h.mailbox, State::Ready, 1, 5);
		Guarded<48> output;
		const auto r = h.mailbox.read(0, output.output().subspan(alignment, 37));
		CHECK(r.status == R::Ok && r.written == 37 && r.eof);
		CHECK(get32(output.output(), alignment + 32) == 0x10203040);
		CHECK(output.output()[alignment + 36] == std::byte{1} && output.intact() && h.intact());
	}
	resetDevice();
	Harness<> h;
	const auto p = emptyPacket(Op::FieldRead, 4);
	start(h.mailbox, p, 1);
	h.mailbox.process(0);
	std::array<std::byte, 37> before{}, after{};
	const auto first = h.mailbox.read(0, before);
	device.current = {999, false};
	const auto second = h.mailbox.read(0, after);
	CHECK(first.eof && second.eof && before == after && get32(after, 32) == 42);
	CHECK(device.reads == 1 && h.intact());
}

static void capacityAndStorage() noexcept
{
	resetDevice();
	const Config value{33, false};
	{
		Harness<24, 5> h;
		const auto p = packet(Op::FieldWrite, 0, value);
		writeIs(h.mailbox.write(0, p, true), R::BufferTooSmall, 0);
		CHECK(callbacks() == 0 && h.intact());
	}
	{
		Harness<29, 4> h;
		const auto p = packet(Op::Service, 0, value);
		start(h.mailbox, p, 1);
		h.mailbox.process(0);
		snapshot(h.mailbox, State::Ready, 1, 0, 6);
		CHECK(callbacks() == 0 && h.intact());
	}
	{
		Harness<29, 4095> h;
		device.serviceStatus = S::Busy;
		const auto p = packet(Op::Service, 3, value);
		start(h.mailbox, p, 1);
		h.mailbox.process(0);
		snapshot(h.mailbox, State::Ready, 1, 0, 6);
		CHECK(callbacks() == 0 && h.intact());
	}
	{
		Harness<24, 4095> h;
		const auto p = emptyPacket(Op::FieldRead, 3);
		start(h.mailbox, p, 1);
		h.mailbox.process(0);
		snapshot(h.mailbox, State::Ready, 1, 0, 6);
		CHECK(callbacks() == 0 && h.intact());
	}
	{
		Harness<4120, 4096, 0> h;
		Big b{};
		b.words.back() = 456;
		const auto p = packet(Op::Command, 3, b);
		start(h.mailbox, p, 1);
		h.mailbox.process(0);
		snapshot(h.mailbox, State::Ready, 1, 0, 7);
		CHECK(callbacks() == 0 && h.workspace.used() == 0 && h.intact());
	}
	device.serviceStatus = S::Ok;
	{
		Harness<> h;
		Big b{};
		b.words.back() = 456;
		const auto p = packet(Op::FieldWrite, 3, b);
		start(h.mailbox, p, 1);
		h.mailbox.process(0);
		snapshot(h.mailbox, State::Ready, 1);
		CHECK(device.current.value == 456 && callbacks() == 1);
		const auto s = packet(Op::Service, 3, value, 2);
		start(h.mailbox, s, 2);
		h.mailbox.process(1);
		snapshot(h.mailbox, State::Ready, 2, 4096);
		CHECK(callbacks() == 2);
		CHECK(h.workspace.used() == 0 && h.intact());
	}
	// A borrowed getter can be called before output-alias refusal. That result is retained.
	resetDevice();
	{
		Guarded<24> request;
		Guarded<model.maxScratch()> scratch;
		ts::Workspace workspace{scratch.output()};
		Output alias{reinterpret_cast<std::byte*>(&device.current), sizeof(device.current)};
		rpc::RpcMailbox mailbox{view, workspace, request.output(), alias};
		const auto p = emptyPacket(Op::FieldRead, 4);
		start(mailbox, p, 1);
		mailbox.process(0);
		snapshot(mailbox, State::Ready, 1, 0, 5);
		CHECK(device.reads == 1 && device.current.value == 42);
		writeIs(mailbox.write(0, p, true), R::Ok, cursor(1, 24), 24, true);
		mailbox.process(1);
		CHECK(device.reads == 1 && request.intact() && scratch.intact());
	}
}

// A separate large Model keeps the ARM fixture's RAM demand independent of this host case.
struct Huge {
	std::array<std::uint8_t, 65536> bytes;
};

struct HugeOwner {
	unsigned calls = 0;
	unsigned sum = 0;

	C run(const Huge& value) noexcept
	{
		++calls;
		sum = value.bytes.front() + value.bytes.back();
		return C::Executed;
	}
};

static void hugeTransfer() noexcept
{
	static HugeOwner owner;
	static constexpr ts::Model hugeModel{
	    ts::group("huge", ts::command<&HugeOwner::run>("Huge", owner))};
	static constexpr auto hugeView = hugeModel.view();
	static Guarded<24 + 65536> storage;
	static Guarded<hugeModel.maxScratch()> scratch;
	static std::array<std::byte, 24 + 65536> packetBytes;
	static std::array<std::byte, 0> result;
	ts::Workspace workspace{scratch.output()};
	rpc::RpcMailbox mailbox{hugeView, workspace, storage.output(), result};
	header(packetBytes, Op::Command, 0, 65536, 1);
	for (unsigned i = 0; i < 65536; ++i)
		packetBytes[24 + i] = std::byte(i & 255);
	owner = {};
	writeIs(mailbox.write(0, Input{packetBytes}.first(65535), false), R::Ok, cursor(1, 65535),
	        65535);
	writeIs(mailbox.write(cursor(1, 65535), Input{packetBytes}.subspan(65535), true), R::Ok,
	        cursor(1, 65560), 25, true);
	CHECK(owner.calls == 0);
	mailbox.process(0);
	snapshot(mailbox, State::Ready, 1);
	CHECK(owner.calls == 1 && owner.sum == 255 && workspace.used() == 0);
	writeIs(mailbox.write(0, Input{packetBytes}.first(65535), false), R::Ok, cursor(1, 65535),
	        65535);
	mailbox.process(1);
	CHECK(owner.calls == 1 && storage.intact() && scratch.intact());
}

static void resourceEnvelope() noexcept
{
	resetDevice();
	Harness<> h;
	const resource::FileSystem files{std::array{resource::file("/telemetry/rpc.bin", h.mailbox)}};
	const auto packetBytes = emptyPacket(Op::Command, 1, 1);
	std::array<std::byte, 64> transport{};
	transport[0] = std::byte{4};
	put(transport, 1, std::uint32_t{0});
	put(transport, 5, std::uint64_t{0});
	transport[13] = std::byte{1};
	put(transport, 14, std::uint16_t{24});
	std::copy(packetBytes.begin(), packetBytes.end(), transport.begin() + 16);
	const auto refused = resource::protocol::process(files.view(), Input{transport}.first(40),
	                                                 Output{transport}.first(13));
	CHECK(refused.status == R::BufferTooSmall && refused.written == 0);
	snapshot(h.mailbox, State::Idle, 0);
	// In-place outer request/reply does not invalidate deferred RPC storage.
	const auto ack =
	    resource::protocol::process(files.view(), Input{transport}.first(40), transport);
	CHECK(ack.status == R::Ok && ack.written == 14 && callbacks() == 0);
	transport.fill(std::byte{0xab});
	h.mailbox.process(0);
	CHECK(callbacks() == 1);
	transport[0] = std::byte{3};
	put(transport, 1, std::uint32_t{0});
	put(transport, 5, std::uint64_t{0});
	const auto shortReply = resource::protocol::process(files.view(), Input{transport}.first(13),
	                                                    Output{transport}.first(43));
	CHECK(shortReply.status == R::BufferTooSmall && shortReply.written == 12);
	CHECK(transport[0] == std::byte{6});
	transport[0] = std::byte{3};
	put(transport, 1, std::uint32_t{0});
	put(transport, 5, std::uint64_t{0});
	const auto reply = resource::protocol::process(files.view(), Input{transport}.first(13),
	                                               Output{transport}.first(44));
	CHECK(reply.status == R::Ok && reply.written == 44 && transport[9] == std::byte{1});
	CHECK(get32(transport, 12) == 0x53505254u && transport[33] == std::byte{4});
	CHECK(h.intact());
}

int main()
{
	wireAndRead();
	fragmentationAndFinal();
	rejectedStarts();
	timeAndSession();
	reentrantRunning();
	modelOperations();
	packetLifetimesAndAlignment();
	capacityAndStorage();
	hugeTransfer();
	resourceEnvelope();
	std::printf("rpc checks=%u failures=%u\n", checks, failures);
	return failures == 0 ? 0 : 1;
}

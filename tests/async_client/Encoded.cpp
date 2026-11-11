/* Complete-frame wire, actual Model bridge and bounded backend lifecycle. MIT. */
#include "EncodedFixture.hpp"
#include <cstdio>
#include <cstring>
#include <type_traits>
#if defined(ENCODED_NO_HEAP)
#include <new>

[[noreturn]] static void heapForbidden() noexcept
{
	std::fputs("ENCODED_HEAP_FORBIDDEN\n", stderr);
	std::_Exit(86);
}

void* operator new(std::size_t)
{
	heapForbidden();
}

void* operator new[](std::size_t)
{
	heapForbidden();
}

void* operator new(std::size_t, std::align_val_t)
{
	heapForbidden();
}

void* operator new[](std::size_t, std::align_val_t)
{
	heapForbidden();
}

void* operator new(std::size_t, const std::nothrow_t&) noexcept
{
	heapForbidden();
}

void* operator new[](std::size_t, const std::nothrow_t&) noexcept
{
	heapForbidden();
}

void* operator new(std::size_t, std::align_val_t, const std::nothrow_t&) noexcept
{
	heapForbidden();
}

void* operator new[](std::size_t, std::align_val_t, const std::nothrow_t&) noexcept
{
	heapForbidden();
}

void operator delete(void*) noexcept
{}

void operator delete[](void*) noexcept
{}

void operator delete(void*, std::size_t) noexcept
{}

void operator delete[](void*, std::size_t) noexcept
{}

void operator delete(void*, std::align_val_t) noexcept
{}

void operator delete[](void*, std::align_val_t) noexcept
{}

void operator delete(void*, std::size_t, std::align_val_t) noexcept
{}

void operator delete[](void*, std::size_t, std::align_val_t) noexcept
{}
#endif

using namespace encoded_fixture;
static unsigned checks = 0, failures = 0;

static void check(bool value, unsigned line) noexcept
{
	++checks;
	if (!value) {
		++failures;
		std::fprintf(stderr, "Encoded.cpp:%u failed (%u)\n", line, checks);
	}
}

#define CHECK(value) check(bool(value), __LINE__)

static wire::ParseResult dispatch(Harness& h, Input frame) noexcept
{
	const auto result = server::dispatchEncoded(modelView, h.workspace, 17, frame, h.reply);
	CHECK(result.status == server::EncodedDispatchStatus::Ok);
	const auto parsed = wire::parse(Input{h.reply}.first(result.written));
	CHECK(parsed && parsed.header.reply);
	return parsed;
}

static void goldenWire() noexcept
{
	const auto payload = configBytes(0x78563412u);
	std::array<std::byte, 37> expected{}, actual{};
	request(expected, wire::Operation::Service, 0x1234abcdu, payload, 5, 0x11223344u, 0x55667788u);
	wire::Header header{};
	header.operation = wire::Operation::Service;
	header.session = 0x11223344u;
	header.requestId = 0x55667788u;
	header.endpoint = 0x1234abcdu;
	header.expectedResponseBytes = 5;
	const auto built = wire::write(actual, header, payload);
	CHECK(built && built.written == 37);
	CHECK(actual == expected);
	const auto parsed = wire::parse(actual);
	CHECK(parsed && parsed.header.session == header.session &&
	      parsed.header.requestId == header.requestId);
	CHECK(parsed.header.endpoint == header.endpoint && parsed.header.operation == header.operation);
	CHECK(parsed.payload.size() == 5 && parsed.payload[4] == std::byte{1});
	CHECK(wire::headerBytes == 32 && wire::magic == 0x31514554u);
	for (std::size_t count = 0; count < expected.size(); ++count)
		CHECK(!wire::parse(Input{expected}.first(count)));
	auto malformed = expected;
	for (const auto offset : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 28u, 29u, 30u, 31u}) {
		malformed = expected;
		malformed[offset] = std::byte{0xff};
		CHECK(!wire::parse(malformed));
	}
	malformed = expected;
	put32(malformed, 8, 0);
	CHECK(!wire::parse(malformed));
	put32(malformed, 8, 1);
	put32(malformed, 12, 0);
	CHECK(!wire::parse(malformed));
	std::array<std::byte, 38> trailing{};
	std::copy(expected.begin(), expected.end(), trailing.begin());
	CHECK(!wire::parse(trailing));
	std::array<std::byte, 36> small{};
	std::fill(small.begin(), small.end(), std::byte{0xa5});
	const auto before = small;
	CHECK(wire::write(small, header, payload).status == wire::Status::BufferTooSmall);
	CHECK(small == before);
	// Payload may start in the header area of the same destination.
	std::array<std::byte, 37> alias{};
	std::copy(payload.begin(), payload.end(), alias.begin());
	CHECK(wire::write(alias, header, Input{alias}.first(5)));
	CHECK(alias == expected);
}

static void headerAliasing() noexcept
{
	struct Storage {
		wire::Header header{};
		std::array<std::byte, wire::headerBytes + 8> padding{};
	};

	wire::Header header{};
	header.reply = true;
	header.operation = wire::Operation::Service;
	header.session = 0x12345678u;
	header.requestId = 0x87654321u;
	header.endpoint = 0xaabbccddu;
	std::array<std::byte, wire::headerBytes> expected{};
	CHECK(wire::writeHeader(expected, header) == wire::Status::Ok);
	CHECK(wire::parse(expected));
	// Aligned native source and byte destinations at/within the same object.
	// Displaced output can overwrite integer fields before they are serialized.
	for (const std::size_t displacement : {0u, 1u, 4u}) {
		Storage storage{};
		storage.header = header;
		auto bytes = std::as_writable_bytes(std::span{&storage, std::size_t{1}});
		std::array<std::byte, sizeof(Storage)> before{};
		std::memcpy(before.data(), bytes.data(), bytes.size());
		const auto output = bytes.subspan(displacement, wire::headerBytes);
		CHECK(wire::writeHeader(output, storage.header) == wire::Status::Ok);
		CHECK(std::equal(output.begin(), output.end(), expected.begin()));
		const auto parsed = wire::parse(output);
		CHECK(parsed && parsed.header.reply && parsed.header.operation == header.operation);
		CHECK(parsed.header.session == header.session &&
		      parsed.header.requestId == header.requestId &&
		      parsed.header.endpoint == header.endpoint);
		CHECK(std::equal(bytes.begin(), bytes.begin() + displacement, before.begin()));
		CHECK(std::equal(bytes.begin() + displacement + wire::headerBytes, bytes.end(),
		                 before.begin() + displacement + wire::headerBytes));
	}
	for (const bool invalid : {false, true}) {
		Storage storage{};
		storage.header = header;
		if (invalid)
			storage.header.session = 0;
		auto bytes = std::as_writable_bytes(std::span{&storage, std::size_t{1}});
		std::array<std::byte, sizeof(Storage)> before{};
		std::memcpy(before.data(), bytes.data(), bytes.size());
		const auto output = invalid ? bytes : bytes.first(wire::headerBytes - 1);
		CHECK(wire::writeHeader(output, storage.header) ==
		      (invalid ? wire::Status::InvalidHeader : wire::Status::BufferTooSmall));
		CHECK(std::equal(bytes.begin(), bytes.end(), before.begin()));
	}
}

static void realModel() noexcept
{
	device = Device{};
	optionalOwner.reset();
	Harness h;
	const auto payload = configBytes(731, false);
	std::array<std::byte, 37> frame{};
	auto count = request(frame, wire::Operation::FieldWrite, 0, payload, 0);
	auto reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == 0 && reply.payload.empty());
	CHECK(device.writes == 1 && device.current.value == 731 && !device.current.enabled);
	count = request(frame, wire::Operation::FieldRead, 0, {}, 5, 17, 2);
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 0 && reply.payload.size() == 5);
	CHECK(std::equal(payload.begin(), payload.end(), reply.payload.begin()));
	CHECK(device.reads == 1);
	count = request(frame, wire::Operation::Command, 0, payload, 0, 17, 3);
	device.commandStatus = ts::CommandResult::Accepted;
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == 1 && reply.payload.empty());
	CHECK(device.commands == 1);
	count = request(frame, wire::Operation::Service, 0, payload, 5, 17, 4);
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == 0 &&
	      reply.payload.size() == 5);
	CHECK(std::equal(payload.begin(), payload.end(), reply.payload.begin()));
	CHECK(device.services == 1);
	device.serviceStatus = ts::ServiceStatus::Busy;
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == 3 && reply.payload.empty());
	CHECK(device.services == 2); // The plain bridge intentionally has no dedup.
	count = request(frame, wire::Operation::Command, 1, {}, 0, 17, 5);
	device.commandStatus = ts::CommandResult::Executed;
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == 0 && device.commands == 2);
	count = request(frame, wire::Operation::Service, 1, {}, 0, 17, 6);
	device.serviceStatus = ts::ServiceStatus::Ok;
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == 0 && reply.payload.empty());
	CHECK(device.services == 3);
	count = request(frame, wire::Operation::FieldWrite, 1, payload, 0);
	const auto before = calls();
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == 2 && calls() == before);
	count = request(frame, wire::Operation::FieldRead, 4, {}, 5);
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 9 && calls() == before);
	count = request(frame, wire::Operation::FieldRead, 0x10000u, {}, 5);
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 4 && calls() == before);
}

static void rejectsBeforeEffects() noexcept
{
	device = Device{};
	Harness h;
	auto payload = configBytes();
	std::array<std::byte, 37> frame{};
	auto count = request(frame, wire::Operation::Service, 0, payload, 4);
	auto reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 5 && calls() == 0);
	count = request(frame, wire::Operation::FieldWrite, 0, Input{payload}.first(4), 0);
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 5 && calls() == 0);
	payload[4] = std::byte{2};
	count = request(frame, wire::Operation::FieldWrite, 0, payload, 0);
	reply = dispatch(h, Input{frame}.first(count));
	CHECK(reply.header.dispatch == 5 && calls() == 0);
	count = request(frame, wire::Operation::FieldRead, 0, {}, 5);
	std::fill(h.reply.begin(), h.reply.end(), std::byte{0xa5});
	const auto untouched = h.reply;
	const auto shortOut = server::dispatchEncoded(
	    modelView, h.workspace, 17, Input{frame}.first(count), Output{h.reply}.first(31));
	CHECK(shortOut.status == server::EncodedDispatchStatus::BufferTooSmall && calls() == 0);
	CHECK(h.reply == untouched);
	const auto noPayloadRoom = server::dispatchEncoded(
	    modelView, h.workspace, 17, Input{frame}.first(count), Output{h.reply}.first(36));
	CHECK(noPayloadRoom && noPayloadRoom.written == 32 && calls() == 0);
	CHECK(wire::parse(Input{h.reply}.first(32)).header.dispatch == 6);
	const auto wrongSession =
	    server::dispatchEncoded(modelView, h.workspace, 18, Input{frame}.first(count), h.reply);
	CHECK(wrongSession.status == server::EncodedDispatchStatus::NotAdmitted && calls() == 0);
	const auto alias =
	    server::dispatchEncoded(modelView, h.workspace, 17, Input{frame}.first(count), frame);
	CHECK(alias.status == server::EncodedDispatchStatus::Overlap && calls() == 0);
	std::copy_n(frame.begin(), count, h.scratch.begin());
	CHECK(
	    server::dispatchEncoded(modelView, h.workspace, 17, Input{h.scratch}.first(count), h.reply)
	        .status == server::EncodedDispatchStatus::Overlap);
	CHECK(calls() == 0);
	for (const auto offset : {0u, 4u, 5u, 6u, 7u, 28u, 29u}) {
		auto bad = frame;
		bad[offset] = std::byte{0xff};
		CHECK(server::dispatchEncoded(modelView, h.workspace, 17, Input{bad}.first(count), h.reply)
		          .status == server::EncodedDispatchStatus::Malformed);
		CHECK(calls() == 0);
	}
	std::array<std::byte, 1> emptyScratch{};
	ts::Workspace tiny{Output{emptyScratch}.first(0)};
	count = request(frame, wire::Operation::FieldRead, 3, {}, ts::wireSize<Big>);
	const auto noScratch =
	    server::dispatchEncoded(modelView, tiny, 17, Input{frame}.first(count), h.reply);
	CHECK(noScratch && wire::parse(Input{h.reply}.first(noScratch.written)).header.dispatch == 7);
	CHECK(calls() == 0);
	// Borrowed native output is discovered only by the getter. Its actual
	// object extent must still reject overlap before any native value write.
	count = request(frame, wire::Operation::FieldRead, 2, {}, 5);
	auto ownerBytes = std::as_writable_bytes(std::span<Device>{&device, 1});
	const auto ownerBefore = device.current;
	const auto borrowed = server::dispatchEncoded(modelView, h.workspace, 17,
	                                              Input{frame}.first(count), ownerBytes.first(37));
	CHECK(borrowed && wire::parse(Input{ownerBytes}.first(borrowed.written)).header.dispatch == 5);
	CHECK(device.current.value == ownerBefore.value &&
	      device.current.enabled == ownerBefore.enabled);
	CHECK(device.reads == 1);
}

static void endpointStatusLayers() noexcept
{
	device = Device{};
	Harness h;
	const auto payload = configBytes();
	std::array<std::byte, 37> frame{};
	for (unsigned code = 0; code <= 5; ++code) {
		device.writeStatus = static_cast<ts::WriteResult>(code);
		const auto count = request(frame, wire::Operation::FieldWrite, 0, payload, 0);
		const auto reply = dispatch(h, Input{frame}.first(count));
		CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == code &&
		      reply.payload.empty());
	}
	for (unsigned code = 0; code <= 7; ++code) {
		device.commandStatus = static_cast<ts::CommandResult>(code);
		const auto count = request(frame, wire::Operation::Command, 0, payload, 0);
		const auto reply = dispatch(h, Input{frame}.first(count));
		CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == code &&
		      reply.payload.empty());
	}
	for (unsigned code = 0; code <= 4; ++code) {
		device.serviceStatus = static_cast<ts::ServiceStatus>(code);
		const auto count = request(frame, wire::Operation::Service, 0, payload, 5);
		const auto reply = dispatch(h, Input{frame}.first(count));
		CHECK(reply.header.dispatch == 0 && reply.header.endpointStatus == code);
		CHECK(reply.payload.size() == (code == 0 ? 5u : 0u));
	}
	device.writeStatus = static_cast<ts::WriteResult>(255);
	device.commandStatus = static_cast<ts::CommandResult>(255);
	for (const auto operation : {wire::Operation::FieldWrite, wire::Operation::Command}) {
		const auto count = request(frame, operation, 0, payload, 0);
		const auto reply = dispatch(h, Input{frame}.first(count));
		CHECK(reply.header.dispatch == 8 && reply.header.endpointStatus == 0 &&
		      reply.payload.empty());
	}
	CHECK(device.writes == 7 && device.commands == 9 && device.services == 5);
}

static client::Packet begin(Harness& h, client::Operation operation, Input payload = {},
                            std::uint32_t responseBytes = 0) noexcept
{
	CHECK(h.backend.start({operation, 0, payload, responseBytes}, h.now) == client::Status::Ok);
	const auto packet = h.backend.nextPacket();
	CHECK(packet && packet.session == 17 && packet.requestId != 0);
	std::copy(packet.bytes.begin(), packet.bytes.end(), h.port.begin());
	return {Input{h.port}.first(packet.bytes.size()), packet.session, packet.requestId};
}

static void backendRoundtrip() noexcept
{
	device = Device{};
	Harness h;
	CHECK(!h.backend.ready() && !h.backend.openSession(0));
	CHECK(h.backend.openSession(17) && h.backend.ready());
	auto payload = configBytes(915);
	const auto packet = begin(h, client::Operation::Service, payload, 5);
	payload = configBytes(1, false);
	CHECK(wire::parse(packet.bytes).payload[0] == std::byte{0x93});
	CHECK(!h.backend.onSendAccepted(packet.session, packet.requestId));
	CHECK(!h.backend.onReply(packet.bytes, packet.session, packet.requestId));
	CHECK(!h.backend.prepareHandoff(18, packet.requestId));
	CHECK(h.backend.prepareHandoff(packet.session, packet.requestId));
	CHECK(!h.backend.prepareHandoff(packet.session, packet.requestId));
	CHECK(!h.backend.nextPacket());
	CHECK(h.backend.onSendAccepted(packet.session, packet.requestId));
	CHECK(!h.backend.onSendAccepted(packet.session, packet.requestId));
	const auto response =
	    server::dispatchEncoded(modelView, h.workspace, 17, packet.bytes, h.reply);
	CHECK(response && device.services == 1);
	auto wrong = h.reply;
	put32(wrong, 16, 9);
	CHECK(!h.backend.onReply(Input{wrong}.first(response.written), 17, packet.requestId));
	CHECK(!h.backend.onReply(Input{h.reply}.first(response.written), 18, packet.requestId));
	CHECK(!h.backend.completed());
	CHECK(h.backend.onReply(Input{h.reply}.first(response.written), 17, packet.requestId));
	CHECK(h.backend.completed() && !h.backend.recoveryRequired());
	const auto result = h.backend.result();
	CHECK(result && result.payload.size() == 5 && result.payload[0] == std::byte{0x93});
	h.reply.fill(std::byte{0xff});
	CHECK(result.payload[0] == std::byte{0x93}); // Backend owns its result copy.
	CHECK(h.backend.start({client::Operation::FieldRead, 0, {}, 5}, h.now) == client::Status::Busy);
	h.backend.releaseResult();
	CHECK(h.backend.ready());
	CHECK(h.backend.result().status == client::Status::Busy);
	const auto read = begin(h, client::Operation::FieldRead, {}, 5);
	CHECK(read.requestId == packet.requestId + 1);
	h.backend.cancel(client::Status::Cancelled);
	CHECK(h.backend.result().status == client::Status::Cancelled && !h.backend.recoveryRequired());
	h.backend.releaseResult();
	CHECK(h.backend.ready());
}

static void uncertaintyAndClock() noexcept
{
	for (const auto operation : {client::Operation::FieldWrite, client::Operation::FieldRead}) {
		Harness h;
		CHECK(h.backend.openSession(17));
		const auto payload = configBytes();
		const auto packet = begin(
		    h, operation, operation == client::Operation::FieldRead ? Input{} : Input{payload},
		    operation == client::Operation::FieldRead ? 5 : 0);
		CHECK(h.backend.prepareHandoff(17, packet.requestId));
		h.now = 10;
		h.backend.process(h.now); // No acceptance callback is required for uncertainty.
		CHECK(h.backend.completed() && h.backend.recoveryRequired());
		CHECK(h.backend.result().status == (operation == client::Operation::FieldRead
		                                        ? client::Status::Timeout
		                                        : client::Status::OutcomeUnknown));
		CHECK(!h.backend.openSession(18));
		CHECK(!h.backend.confirmSessionClosed(18));
		h.backend.releaseResult();
		CHECK(!h.backend.ready());
		CHECK(h.backend.confirmSessionClosed(17));
		CHECK(!h.backend.ready() && !h.backend.openSession(17));
		CHECK(h.backend.openSession(18) && h.backend.ready());
		CHECK(!h.backend.onReply(packet.bytes, 17, packet.requestId));
	}
	Harness h;
	CHECK(h.backend.openSession(17));
	h.now = UINT32_MAX - 5;
	const auto packet = begin(h, client::Operation::FieldRead, {}, 5);
	h.now = 3;
	h.backend.process(h.now);
	CHECK(!h.backend.completed());
	h.now = 4;
	CHECK(!h.backend.prepareHandoff(17, packet.requestId)); // Fresh clock gate.
	CHECK(h.backend.result().status == client::Status::Timeout && !h.backend.recoveryRequired());
	h.backend.releaseResult();
	h.now = 100;
	const auto second = begin(h, client::Operation::FieldRead, {}, 5);
	CHECK(h.backend.prepareHandoff(17, second.requestId));
	std::array<std::byte, 32> malformed{};
	CHECK(h.backend.onReply(malformed, 17, second.requestId));
	CHECK(h.backend.result().status == client::Status::MalformedReply &&
	      h.backend.recoveryRequired());
	CHECK(!h.backend.onSendAccepted(17, second.requestId));
	h.backend.releaseResult();
	CHECK(h.backend.confirmSessionClosed(17));
	CHECK(h.backend.openSession(18));
	h.backend.close();
	CHECK(!h.backend.ready());
}

static void cancellationAndCapacity() noexcept
{
	for (const auto operation : {client::Operation::FieldWrite, client::Operation::FieldRead}) {
		for (const bool disconnect : {false, true}) {
			Harness h;
			CHECK(h.backend.openSession(17));
			const auto payload = configBytes();
			const auto packet = begin(
			    h, operation, operation == client::Operation::FieldRead ? Input{} : Input{payload},
			    operation == client::Operation::FieldRead ? 5 : 0);
			CHECK(h.backend.prepareHandoff(17, packet.requestId));
			if (disconnect)
				h.backend.close();
			else
				h.backend.cancel(client::Status::Cancelled);
			CHECK(h.backend.completed() && h.backend.recoveryRequired());
			CHECK(h.backend.result().status == (operation == client::Operation::FieldWrite
			                                        ? client::Status::OutcomeUnknown
			                                    : disconnect ? client::Status::SessionLost
			                                                 : client::Status::Cancelled));
			CHECK(!h.backend.nextPacket() && !h.backend.onSendAccepted(17, packet.requestId));
		}
	}
	Harness h;
	CHECK(h.backend.start({client::Operation::FieldRead, 0, {}, 5}, 0) ==
	      client::Status::NotAdmitted);
	CHECK(h.backend.openSession(17));
	h.tx.fill(std::byte{0xa5});
	const auto before = h.tx;
	const auto payload = configBytes();
	CHECK(h.backend.start({client::Operation::FieldRead, 0, payload, 5}, 0) ==
	      client::Status::InvalidArgument);
	CHECK(h.tx == before);
	// 7 is the client-only Resource family; this backend was not built to admit it.
	CHECK(h.backend.start({client::Operation::Resource, 0, {}, 0}, 0) ==
	      client::Status::Unsupported);
	CHECK(h.backend.start({static_cast<client::Operation>(8), 0, {}, 0}, 0) ==
	      client::Status::InvalidArgument);
	CHECK(h.backend.start({client::Operation::Service, 0, payload, 1025}, 0) ==
	      client::Status::BufferTooSmall);
	CHECK(h.backend.start({client::Operation::FieldRead, 0, {}, 5, 0}, 0) ==
	      client::Status::InvalidArgument);
	CHECK(h.backend.start({client::Operation::FieldRead, 0, {}, 5, 0x80000000u}, 0) ==
	      client::Status::InvalidArgument);
	const auto packet = begin(h, client::Operation::FieldRead, {}, 5);
	CHECK(packet.requestId == 1); // Failed starts consumed no request identity.
	h.backend.close();
	CHECK(h.backend.result().status == client::Status::SessionLost &&
	      !h.backend.recoveryRequired());
	CHECK(!h.backend.prepareHandoff(17, packet.requestId));
	h.backend.releaseResult();
	CHECK(!h.backend.ready() && h.backend.openSession(UINT32_MAX));
	h.backend.close();
	CHECK(!h.backend.openSession(1) && !h.backend.openSession(UINT32_MAX));
	// Overall deadline was t=102; activation at101 leaves only1ms, while
	// this backend's ordinary transaction timeout is10ms. A delayed port
	// must not hand off the copied packet using the longer backend timeout.
	Harness shortBudget;
	CHECK(shortBudget.backend.openSession(17));
	shortBudget.now = 101;
	CHECK(shortBudget.backend.start({client::Operation::Command, 0, payload, 0, 1},
	                                shortBudget.now) == client::Status::Ok);
	const auto delayed = shortBudget.backend.nextPacket();
	CHECK(delayed);
	std::copy(delayed.bytes.begin(), delayed.bytes.end(), shortBudget.port.begin());
	shortBudget.now = 102;
	CHECK(!shortBudget.backend.prepareHandoff(17, delayed.requestId));
	CHECK(shortBudget.backend.result().status == client::Status::Timeout);
	CHECK(!shortBudget.backend.recoveryRequired() && !shortBudget.backend.nextPacket());
}

int main(int argc, char** argv)
{
#if defined(ENCODED_NO_HEAP)
	if (argc == 2 && std::strcmp(argv[1], "--allocation-control") == 0)
		(void)::operator new(1);
#else
	(void)argc;
	(void)argv;
#endif
	static_assert(!std::is_copy_constructible_v<::telemetry::integration::encoded::EncodedBackend>);
	static_assert(!std::is_move_constructible_v<::telemetry::integration::encoded::EncodedBackend>);
	goldenWire();
	headerAliasing();
	realModel();
	rejectsBeforeEffects();
	endpointStatusLayers();
	backendRoundtrip();
	uncertaintyAndClock();
	cancellationAndCapacity();
	std::printf("encoded checks=%u failures=%u\n", checks, failures);
	return failures == 0 ? 0 : 1;
}

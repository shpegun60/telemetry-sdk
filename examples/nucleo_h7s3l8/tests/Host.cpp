/* Actual board Protocol/ServerLoop and real COBS fixed pools, no HAL. MIT. */
#include "ServerLoop.hpp"
#include <resource/protocol/Packets.hpp>
#include <telemetry/runtime/RuntimeSchema.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
unsigned checks = 0;
#define CHECK(value)                                                                               \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(value)) {                                                                            \
			std::fprintf(stderr, "H7S host check %u line %d: %s\n", checks, __LINE__, #value);     \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)
using Link = cobs::Endpoint<wire::Pool<4, 1>, telemetry::integration::serial::FrameFormat>;
namespace rc = resource::protocol::client;
namespace ew = telemetry::protocol::encoded;
using Input = resource::Input;

struct Physical {
	bool busy = false, failNext = false;
	std::span<const std::uint8_t> borrowed;
	Link* peer = nullptr;

	bool tx_busy() const noexcept
	{
		return busy;
	}

	bool send(std::span<const std::uint8_t> bytes) noexcept
	{
		if (busy || failNext) {
			failNext = false;
			return false;
		}
		busy = true;
		borrowed = bytes;
		return true;
	}

	void complete() noexcept
	{
		CHECK(busy && !borrowed.empty());
		for (const auto byte : borrowed)
			peer->consume({&byte, 1});
		borrowed = {};
		busy = false; // models actual UART TC, not merely DMA count == 0
	}
};

struct Feeder {
	enum Mode {
		Valid,
		Corrupt,
		Truncated
	} mode = Valid;

	Link* peer;

	bool busy() const noexcept
	{
		return false;
	}

	bool send(std::span<const std::uint8_t> bytes) noexcept
	{
		for (std::size_t i = 0; i < bytes.size(); ++i) {
			if (mode == Truncated && i + 1 == bytes.size())
				break;
			auto byte = bytes[i];
			// TEQ1's four nonzero magic bytes occupy an unchanged COBS data run.
			// Alter data, not a COBS code or delimiter, to require a CRC failure.
			if (mode == Corrupt && i + 3 < bytes.size() && bytes[i] == 'T' && bytes[i + 1] == 'E' &&
			    bytes[i + 2] == 'Q' && bytes[i + 3] == '1')
				byte ^= 1;
			peer->consume({&byte, 1});
		}
		mode = Valid;
		return true;
	}
};

struct Harness {
	Link board, host;
	Physical uart{false, false, {}, &host};
	Feeder source{Feeder::Valid, &board};
	nucleo_demo::ServerLoop<Physical, Link> loop{uart, board};

	Harness()
	{
		CHECK(board.bind(tiny::bind<&Physical::send>(uart), tiny::bind<&Physical::tx_busy>(uart)));
		CHECK(host.bind(tiny::bind<&Feeder::send>(source), tiny::bind<&Feeder::busy>(source)));
		nucleo_demo::status = {0, 0, 0, 0, true};
	}

	void request(Input bytes)
	{
		auto message = host.make_message(bytes.size());
		CHECK(message && message.append_bytes(
		                     {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}));
		CHECK(host.send(message) == wire::SendResult::Sent);
		host.poll(0);
	}

	Link::Packet reply()
	{
		CHECK(loop.step(30) == nucleo_demo::Action::Dispatched);
		CHECK(loop.step(31) == nucleo_demo::Action::Sent);
		uart.complete();
		board.poll(32);
		auto result = host.pop_packet();
		CHECK(bool(result));
		return result;
	}
};

Input input(const Link::Packet& packet)
{
	return {reinterpret_cast<const std::byte*>(packet.data().data()), packet.data().size()};
}

std::array<std::byte, 32> readRequest(std::uint32_t session, std::uint32_t id)
{
	std::array<std::byte, 32> frame{};
	ew::Header header{};
	header.operation = ew::Operation::FieldRead;
	header.session = session;
	header.requestId = id;
	header.endpoint = 65536;
	header.expectedResponseBytes = 8;
	CHECK(ew::writeHeader(frame, header) == ew::Status::Ok);
	return frame;
}

void orderedBarrier()
{
	Harness h;
	std::array<std::byte, 9> list{};
	CHECK(rc::makeList(list, 0).status == rc::BuildStatus::Ok);
	h.request(list);
	CHECK(h.loop.step(1) == nucleo_demo::Action::Dispatched);
	CHECK(h.loop.step(2) == nucleo_demo::Action::Sent);
	CHECK(h.uart.tx_busy() && h.board.tx_active());
	const auto oldRead = readRequest(10, 1);
	h.request(oldRead);
	h.request(list); // fresh bootstrap must remain behind the old read and TX
	const auto before = nucleo_demo::status.requests;
	for (unsigned i = 0; i < 100; ++i)
		CHECK(h.loop.step(3) == nucleo_demo::Action::Busy);
	CHECK(nucleo_demo::status.requests == before && nucleo_demo::status.fieldReads == 0);
	h.uart.complete();
	CHECK(!h.uart.tx_busy() && h.board.tx_active());
	CHECK(h.loop.step(4) == nucleo_demo::Action::Busy); // COBS lease independently guards dequeue
	{
		const auto packet = h.host.pop_packet();
		CHECK(bool(rc::parseList(input(packet))));
	}
	h.board.poll(5);
	CHECK(h.loop.step(6) == nucleo_demo::Action::Dispatched);
	CHECK(nucleo_demo::status.fieldReads == 1 && nucleo_demo::status.requests == before + 1);
	CHECK(h.loop.step(7) == nucleo_demo::Action::Sent);
	CHECK(h.loop.step(8) == nucleo_demo::Action::Busy);
	h.uart.complete();
	{
		const auto packet = h.host.pop_packet();
		const auto reply = ew::parse(input(packet));
		CHECK(reply && reply.header.requestId == 1 && reply.header.session == 10);
		CHECK(reply.payload.size() == 8);
		for (const auto byte : reply.payload)
			CHECK(byte == std::byte{0xff});
	}
	CHECK(h.loop.step(9) == nucleo_demo::Action::Busy);
	h.board.poll(10);
	CHECK(h.loop.step(11) == nucleo_demo::Action::Dispatched); // only now LIST is allowed
	CHECK(nucleo_demo::status.requests == before + 2);
	CHECK(h.loop.step(12) == nucleo_demo::Action::Sent);
	h.uart.complete();
	h.board.poll(13);
	{
		const auto packet = h.host.pop_packet();
		CHECK(bool(rc::parseList(input(packet))));
	}
	CHECK(h.loop.step(14) == nucleo_demo::Action::Idle);
	const auto newRead = readRequest(22, 1); // fresh host generation; no TCP-style pin
	h.request(newRead);
	{
		const auto packet = h.reply();
		const auto reply = ew::parse(input(packet));
		CHECK(reply && reply.header.session == 22);
	}
}

// The descriptor read over the link is a group tree (v3.1) that names its
// enum and structs, as the Inspector shows them; with TELEMETRY_TYPE_NAMES 0
// it is the same tree, unnamed.
void typeNames(std::span<const std::byte> bytes)
{
	namespace rt = telemetry::runtime;
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 32> members{};
	std::array<rt::RuntimeEnumEntry, 8> enums{};
	std::array<rt::RuntimeCatalog, 8> catalogs{};
	std::array<rt::RuntimeEndpoint, 8> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(bytes, workspace);
	CHECK(bool(parsed));
	const auto& schema = parsed.schema;
	const auto* voltage = schema.findField("measure", "Voltage");
	const auto* status = schema.findField("device", "Status");
	const auto* probe = schema.findField("device", "Probe");
	CHECK(voltage && status && probe);
	CHECK(schema.minorVersion() == 1 && schema.groupTree());
#if TELEMETRY_TYPE_NAMES
	CHECK(schema.typeName(voltage->typeId) == "nucleo_demo::Voltage");
	CHECK(schema.typeName(status->typeId) == "nucleo_demo::Status");
	CHECK(schema.typeName(probe->typeId) == "nucleo_demo::Probe");
	const auto* mode = schema.findType("nucleo_demo::Mode");
	CHECK(mode && mode->kind == telemetry::TypeKind::Enum && mode->enumCount == 2);
	const auto entries = schema.enumEntries(mode->id);
	CHECK(entries.size() == 2 && entries[0].name == "Idle" && entries[1].name == "Active");
#else
	CHECK(schema.typeName(voltage->typeId).empty() && schema.typeName(probe->typeId).empty());
	CHECK(!schema.findType("nucleo_demo::Mode"));
#endif
}

void descriptorAndReadOnly()
{
	Harness h;
	std::array<std::byte, 16> request{};
	const auto list = rc::makeList(request, 0);
	h.request(Input{request}.first(list.written));
	{
		const auto packet = h.reply();
		const auto reply = rc::parseList(input(packet));
		CHECK(reply && reply.response.paths.size() == 2 && reply.response.eof);
		auto i = reply.response.paths.begin();
		CHECK(*i++ == "/device/version.bin");
		CHECK(*i == "/telemetry/descriptor.bin");
	}
	const auto stat = rc::makeStat(request, 1);
	h.request(Input{request}.first(stat.written));
	{
		const auto packet = h.reply();
		const auto reply = rc::parseStat(input(packet));
		CHECK(reply && reply.response.size == nucleo_demo::packedDescriptor.size());
	}
	const auto read = rc::makeRead(request, 1, 0);
	h.request(Input{request}.first(read.written));
	{
		const auto packet = h.reply();
		const auto reply = rc::parseRead(input(packet));
		CHECK(reply && reply.response.eof &&
		      reply.response.data.size() == nucleo_demo::packedDescriptor.size());
		CHECK(std::memcmp(reply.response.data.data(), nucleo_demo::packedDescriptor.data(),
		                  reply.response.data.size()) == 0);
		typeNames(reply.response.data);
	}
	// Every family has a row per group; no group holds a Command or Service.
	const auto view = nucleo_demo::model.view();
	CHECK(view.commandCatalogCount == view.fieldCatalogCount &&
	      view.commands.catalogs()[0].count == 0 && view.services.catalogs()[1].count == 0);
	CHECK(view.commands.find(0) == nullptr && view.services.find(0) == nullptr);
	const auto fieldReads = nucleo_demo::status.fieldReads;
	for (const auto op :
	     {ew::Operation::FieldWrite, ew::Operation::Command, ew::Operation::Service}) {
		std::array<std::byte, 48> frame{};
		ew::Header header{};
		header.session = 1;
		header.requestId = 1;
		header.operation = op;
		std::array<std::byte, 12> payload{};
		const auto built =
		    ew::write(frame, header, op == ew::Operation::FieldWrite ? Input{payload} : Input{});
		CHECK(bool(built));
		h.request(Input{frame}.first(built.written));
		CHECK(h.loop.step(50) == nucleo_demo::Action::Refused);
		CHECK(!h.uart.tx_busy() && !h.board.tx_active() && !h.host.has_packet());
	}
	request[0] = std::byte{4}; // ordinary Resource WRITE is refused before provider dispatch
	h.request(request);
	CHECK(h.loop.step(50) == nucleo_demo::Action::Refused);
	CHECK(nucleo_demo::status.refused == 4 && nucleo_demo::status.fieldReads == fieldReads);
	CHECK(nucleo_demo::counter() == UINT64_MAX && nucleo_demo::status.demonstration);
}

void damagedFramesAndSendFailure()
{
	Harness h;
	const auto request = readRequest(33, 1);
	h.source.mode = Feeder::Corrupt;
	h.request(request);
	CHECK(h.loop.step(0) == nucleo_demo::Action::Idle && nucleo_demo::status.requests == 0);
	CHECK(h.board.stats().rx.crc_errors == 1 && h.board.stats().rx.malformed == 0);
	h.source.mode = Feeder::Truncated;
	h.request(request);
	CHECK(h.loop.step(0) == nucleo_demo::Action::Idle && nucleo_demo::status.requests == 0);
	h.board.notify_gap();
	const std::uint8_t delimiter = 0;
	h.board.consume({&delimiter, 1});
	h.request(request);
	CHECK(h.loop.step(0) == nucleo_demo::Action::Dispatched);
	h.uart.failNext = true;
	CHECK(h.loop.step(0) == nucleo_demo::Action::TxFailed);
	CHECK(h.loop.step(0) == nucleo_demo::Action::Idle && !h.uart.tx_busy());
	CHECK(nucleo_demo::status.fieldReads == 1); // no automatic execution/reply replay
}
} // namespace

void* operator new(std::size_t)
{
	std::abort();
}

void* operator new[](std::size_t)
{
	std::abort();
}

void operator delete(void*) noexcept
{
	std::abort();
}

void operator delete[](void*) noexcept
{
	std::abort();
}

void operator delete(void*, std::size_t) noexcept
{
	std::abort();
}

void operator delete[](void*, std::size_t) noexcept
{
	std::abort();
}

int main()
{
	orderedBarrier();
	descriptorAndReadOnly();
	damagedFramesAndSendFailure();
	std::printf("NUCLEO H7S host checks=%u passed, fixed pools, hardware=0\n", checks);
}

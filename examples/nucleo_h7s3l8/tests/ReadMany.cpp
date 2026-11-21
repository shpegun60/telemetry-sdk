/* Actual optional board Model, discovery epochs and COBS ownership. MIT. */
#include "ServerLoop.hpp"
#include <resource/protocol/Packets.hpp>
#include <telemetry/encoded/ReadMany.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

namespace {
unsigned checks = 0;
#define CHECK(value)                                                                               \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(value)) {                                                                            \
			std::fprintf(stderr, "H7S ReadMany check %u line %d: %s\n", checks, __LINE__, #value); \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)
namespace rc = resource::protocol::client;
namespace ew = telemetry::protocol::encoded;
namespace rm = ew::read_many;
namespace demo = nucleo_demo;
using Input = resource::Input;
using Output = resource::Output;
using Link = cobs::Endpoint<wire::Pool<4, 1>, telemetry::integration::serial::FrameFormat>;

static_assert(!std::is_copy_constructible_v<demo::Protocol> &&
              !std::is_move_constructible_v<demo::Protocol>);
static_assert(ew::headerBytes + rm::maxRequestPayload == 162 &&
              ew::headerBytes + rm::maxReplyPayload == 546);
static_assert(telemetry::integration::serial::maxBodyBytes >=
              ew::headerBytes + rm::maxReplyPayload);
static_assert(telemetry::integration::serial::FrameFormat::tx_storage_size_for_capacity(162) ==
                  168 &&
              telemetry::integration::serial::FrameFormat::tx_storage_size_for_capacity(546) ==
                  554);
static_assert(telemetry::integration::serial::exchangeTimeoutMs(162, 546, 115200, 100) == 163);

std::uint64_t load(Input bytes)
{
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < bytes.size(); ++i)
		value |= std::uint64_t{std::to_integer<unsigned>(bytes[i])} << (8 * i);
	return value;
}

struct Frame {
	std::array<std::byte, 256> bytes{};
	std::size_t size = 0;

	Input input() const noexcept
	{
		return Input{bytes}.first(size);
	}
};

Frame rawBatch(Input body, std::uint32_t session = 1, std::uint32_t budget = rm::maxReplyPayload)
{
	Frame frame;
	ew::Header header{};
	header.operation = ew::Operation::ReadMany;
	header.session = session;
	header.requestId = 17;
	header.expectedResponseBytes = budget;
	const auto built = ew::write(frame.bytes, header, body);
	CHECK(bool(built));
	frame.size = built.written;
	return frame;
}

Frame batch(std::span<const std::uint32_t> ids, std::uint32_t session = 1,
            std::uint32_t budget = rm::maxReplyPayload)
{
	std::array<std::byte, rm::maxRequestPayload> body{};
	const auto built = rm::writeRequest(body, ids);
	CHECK(bool(built));
	return rawBatch(Input{body}.first(built.written), session, budget);
}

Frame field(std::uint32_t id, std::uint32_t session = 1)
{
	Frame frame;
	ew::Header header{};
	header.session = session;
	header.requestId = 18;
	header.endpoint = id;
	header.expectedResponseBytes = demo::model.view().fields.find(id)->wireBytes;
	CHECK(ew::writeHeader(frame.bytes, header) == ew::Status::Ok);
	frame.size = ew::headerBytes;
	return frame;
}

std::array<std::uint32_t, 32> ids()
{
	std::array<std::uint32_t, 32> result{};
	std::size_t n = 0;
	for (std::uint32_t row = 0; row < 8; ++row)
		for (std::uint32_t group = 2; group < 6; ++group)
			if (group != 5 || row != 7)
				result[n++] = (group << 16) | row;
	result[n++] = 65536;
	CHECK(n == result.size());
	return result;
}

std::uint64_t expected(std::uint32_t id)
{
	const auto row = id & 0xffffu;
	switch (id >> 16) {
		case 1:
			return UINT64_MAX;
		case 2:
			return 0x10203040u + row * 0x01010101u;
		case 3:
			return UINT64_MAX - std::uint64_t{row} * UINT64_C(0x0102030405060708);
		case 4:
			return row % 2 == 0;
		case 5:
			return static_cast<std::uint16_t>(-2 - static_cast<std::int16_t>(row));
	}
	std::abort();
}

std::array<std::byte, 9> list(std::uint64_t cursor = 0)
{
	std::array<std::byte, 9> request{};
	CHECK(rc::makeList(request, cursor).status == rc::BuildStatus::Ok);
	return request;
}

rm::Capability capability(demo::Protocol& protocol)
{
	std::array<std::byte, 16> request{};
	std::array<std::byte, 64> response{};
	auto built = rc::makeStat(request, 2);
	auto size = protocol.dispatch(Input{request}.first(built.written), response, 1);
	const auto stat = rc::parseStat(Input{response}.first(size));
	CHECK(stat && stat.response.size == rm::capabilityBytes);
	built = rc::makeRead(request, 2, 0);
	size = protocol.dispatch(Input{request}.first(built.written), response, 2);
	const auto read = rc::parseRead(Input{response}.first(size));
	CHECK(read && read.response.eof && read.response.data.size() == rm::capabilityBytes);
	const auto cap = rm::parseCapability(read.response.data);
	CHECK(bool(cap));
	CHECK(cap.value.fingerprint == demo::descriptor.fingerprint());
	CHECK(cap.value.itemLimit == 32 && cap.value.requestLimit == 130 &&
	      cap.value.replyLimit == 514);
	return cap.value;
}

void emptyCapability(demo::Protocol& protocol)
{
	std::array<std::byte, 16> request{}, response{};
	const auto built = rc::makeStat(request, 2);
	const auto written = protocol.dispatch(Input{request}.first(built.written), response, 0);
	const auto stat = rc::parseStat(Input{response}.first(written));
	CHECK(stat && stat.response.size == 0 && stat.response.flags == resource::FileFlag::Readable);
}

void discover(demo::Protocol& protocol, std::uint32_t expectedSession)
{
	std::array<std::byte, 128> response{};
	const auto written = protocol.dispatch(list(), response, 0);
	const auto parsed = rc::parseList(Input{response}.first(written));
	CHECK(parsed && parsed.response.eof && parsed.response.paths.size() == 3);
	auto path = parsed.response.paths.begin();
	CHECK(*path++ == "/device/version.bin");
	CHECK(*path++ == "/telemetry/descriptor.bin");
	CHECK(*path == "/telemetry/read-many.bin");
	CHECK(capability(protocol).session == expectedSession);
}

void descriptorAndEpochs()
{
	demo::Protocol protocol;
	demo::scalarFixture.calls = 0;
	emptyCapability(protocol);
	std::array<std::byte, 2048> response{};
	const auto all = ids();
	CHECK(protocol.dispatch(batch(all).input(), response, 0) == 0);
	CHECK(protocol.dispatch(field(65536).input(), response, 0) == 0);
	CHECK(demo::scalarFixture.calls == 0);
	const auto initial = list();
	CHECK(protocol.dispatch(Input{initial}.first(8), response, 0) == 1);
	emptyCapability(protocol);
	CHECK(protocol.dispatch(initial, {}, 0) == 0);
	emptyCapability(protocol);
	CHECK(protocol.dispatch(initial, Output{response}.first(12), 0) == 12);
	emptyCapability(protocol);
	CHECK(protocol.dispatch(initial, Output{response}.first(32), 0) == 12);
	emptyCapability(protocol); // Capacity cannot hold even the first whole path.
	CHECK(protocol.dispatch(list(4), response, 0) == 12);
	emptyCapability(protocol);
	// A first page with progress commits even if subsequent LIST pages remain.
	const auto first = protocol.dispatch(initial, Output{response}.first(40), 0);
	const auto firstPage = rc::parseList(Input{response}.first(first));
	CHECK(firstPage && !firstPage.response.eof && firstPage.response.next == 1);
	CHECK(capability(protocol).session == 1);
	CHECK(protocol.dispatch(list(1), response, 0) > 12);
	CHECK(capability(protocol).session == 1);
	std::array<std::byte, 16> request{};
	auto built = rc::makeStat(request, 1);
	auto written = protocol.dispatch(Input{request}.first(built.written), response, 0);
	const auto stat = rc::parseStat(Input{response}.first(written));
	CHECK(stat && stat.response.size == demo::packedDescriptor.size());
	std::array<std::byte, demo::packedDescriptor.size()> received{};
	resource::Cursor cursor = 0;
	while (cursor < received.size()) {
		built = rc::makeRead(request, 1, cursor);
		written =
		    protocol.dispatch(Input{request}.first(built.written), Output{response}.first(64), 0);
		const auto read = rc::parseRead(Input{response}.first(written));
		CHECK(read && !read.response.data.empty());
		CHECK(read.response.next == cursor + read.response.data.size());
		std::copy(read.response.data.begin(), read.response.data.end(), received.begin() + cursor);
		cursor = read.response.next;
		CHECK(read.response.eof == (cursor == received.size()));
		CHECK(capability(protocol).session == 1);
	}
	CHECK(received == demo::packedDescriptor);
	CHECK(demo::model.view().fields.count() == 6);
	unsigned count = 0, readableScalars = 0;
	demo::model.view().fields.forEachEntry(
	    [&](std::uint32_t id, std::string_view, const telemetry::FieldEntry& entry) {
		    ++count;
		    CHECK(entry.write == nullptr);
		    if ((id == 65536 || (id >> 16) >= 2) && id != 0x50007)
			    ++readableScalars;
	    });
	CHECK(count == 36 && readableScalars == 32);
	CHECK(std::strcmp(demo::model.view().fields.find(0)->name, "Voltage") == 0);
	CHECK(std::strcmp(demo::model.view().fields.find(65536)->name, "Counter") == 0);
	CHECK(std::strcmp(demo::model.view().fields.find(65537)->name, "Status") == 0);
	CHECK(std::strcmp(demo::model.view().fields.find(65538)->name, "Probe") == 0);
	discover(protocol, 2);
	CHECK(protocol.dispatch(field(65536, 1).input(), response, 0) == 0);
	CHECK(protocol.dispatch(batch(all, 1).input(), response, 0) == 0);
	CHECK(demo::scalarFixture.calls == 0);
	CHECK(protocol.dispatch(field(65536, 2).input(), response, 0) == 40);
	CHECK(capability(protocol).session == 2);

	demo::Protocol exhausted{UINT32_MAX - 1};
	emptyCapability(exhausted);
	discover(exhausted, UINT32_MAX);
	CHECK(exhausted.dispatch(field(65536, UINT32_MAX).input(), response, 0) == 40);
	CHECK(exhausted.dispatch(initial, response, 0) > 12);
	emptyCapability(exhausted);
	CHECK(exhausted.dispatch(field(65536, UINT32_MAX).input(), response, 0) == 0);
	CHECK(exhausted.dispatch(batch(all, UINT32_MAX).input(), response, 0) == 0);
	CHECK(exhausted.dispatch(initial, response, 0) > 12);
	emptyCapability(exhausted);
}

void batchesAndPreflight()
{
	demo::Protocol protocol;
	discover(protocol, 1);
	const auto all = ids();
	std::array<std::byte, 2048> response{};
	for (std::size_t count : {8u, 12u, 16u, 32u}) {
		const auto selected = std::span{all}.first(count);
		demo::scalarFixture.calls = 0;
		const auto written = protocol.dispatch(batch(selected).input(), response, 10);
		const auto reply = ew::parse(Input{response}.first(written));
		CHECK(reply && reply.header.reply && reply.header.session == 1 &&
		      reply.header.requestId == 17 && reply.header.dispatch == 0 &&
		      reply.header.operation == ew::Operation::ReadMany);
		const auto parsed = rm::parseReply(reply.payload, selected);
		CHECK(parsed && parsed.count == count);
		CHECK(demo::scalarFixture.calls == count - (count == 32 ? 1 : 0));
		std::array<std::array<std::byte, 8>, 32> owned{};
		std::array<std::size_t, 32> lengths{};
		for (std::size_t i = 0; i < count; ++i) {
			CHECK(parsed.items[i].id == selected[i] &&
			      parsed.items[i].status == rm::ItemStatus::Ok);
			CHECK(load(parsed.items[i].payload) == expected(selected[i]));
			lengths[i] = parsed.items[i].payload.size();
			CHECK(lengths[i] == demo::model.view().fields.find(selected[i])->wireBytes);
			std::copy(parsed.items[i].payload.begin(), parsed.items[i].payload.end(),
			          owned[i].begin());
		}
		// Own the batch bytes before reusing response, then compare each FieldRead.
		for (std::size_t i = 0; i < count; ++i) {
			const auto one = protocol.dispatch(field(selected[i]).input(), response, 11);
			const auto baseline = ew::parse(Input{response}.first(one));
			CHECK(baseline && baseline.header.dispatch == 0 &&
			      baseline.payload.size() == lengths[i]);
			CHECK(std::equal(baseline.payload.begin(), baseline.payload.end(), owned[i].begin()));
		}
	}
	const std::array<std::uint32_t, 5> mixed{0x20000, 0xffffffffu, 0, 0x50007, 0x40001};
	demo::scalarFixture.calls = 0;
	auto written = protocol.dispatch(batch(mixed).input(), response, 0);
	auto reply = ew::parse(Input{response}.first(written));
	CHECK(reply && reply.header.dispatch == 0);
	const auto parsed = rm::parseReply(reply.payload, mixed);
	CHECK(parsed && parsed.count == mixed.size());
	const std::array statuses{rm::ItemStatus::Ok, rm::ItemStatus::NotFound,
	                          rm::ItemStatus::UnsupportedType, rm::ItemStatus::Unavailable,
	                          rm::ItemStatus::Ok};
	for (std::size_t i = 0; i < mixed.size(); ++i) {
		CHECK(parsed.items[i].status == statuses[i]);
		if (statuses[i] != rm::ItemStatus::Ok)
			CHECK(parsed.items[i].payload.empty());
	}
	CHECK(load(parsed.items[0].payload) == expected(mixed[0]) &&
	      load(parsed.items[4].payload) == expected(mixed[4]) && demo::scalarFixture.calls == 2);
	const auto before = demo::scalarFixture.calls;
	const auto refused = [&](const Frame& frame) {
		const auto size = protocol.dispatch(frame.input(), response, 0);
		const auto failure = ew::parse(Input{response}.first(size));
		CHECK(failure && failure.header.dispatch != 0 && failure.payload.empty());
		CHECK(demo::scalarFixture.calls == before);
	};
	refused(batch(all, 1, 10)); // Full preflight must precede the first getter.
	std::array<std::byte, 134> invalid{};
	refused(rawBatch(Input{invalid}.first(2))); // Zero items.
	invalid[0] = std::byte{33};
	refused(rawBatch(invalid)); // Beyond both the count and request capacity.
	invalid[0] = std::byte{2};
	refused(rawBatch(Input{invalid}.first(10))); // Duplicate ID 0.
	invalid[0] = std::byte{1};
	refused(rawBatch(Input{invalid}.first(5))); // Truncated item ID.
	auto trailing = batch(std::span{all}.first(1));
	trailing.bytes[16] = std::byte{1}; // Nonzero endpoint violates ReadMany envelope.
	CHECK(protocol.dispatch(trailing.input(), response, 0) == 0);
	CHECK(demo::scalarFixture.calls == before);
	const auto valid = batch(all);
	CHECK(protocol.dispatch(valid.input(), Output{response}.first(31), 0) == 0);
	CHECK(demo::scalarFixture.calls == before);
	const std::array<std::byte, 16> writeResource{std::byte{4}};
	CHECK(protocol.dispatch(writeResource, response, 0) == 0);
	for (auto op : {ew::Operation::FieldWrite, ew::Operation::Command, ew::Operation::Service}) {
		Frame mutation;
		ew::Header header{};
		header.operation = op;
		header.session = 1;
		header.requestId = 1;
		CHECK(ew::writeHeader(mutation.bytes, header) == ew::Status::Ok);
		mutation.size = ew::headerBytes;
		CHECK(protocol.dispatch(mutation.input(), response, 0) == 0);
	}
	CHECK(demo::scalarFixture.calls == before);
}

void readErrorIsolated()
{
	// Deliberate host-only faulty erased row. The firmware's typed scalar
	// getters cannot return short/partial failures through their native API.
	auto view = demo::model.view();
	std::array<telemetry::FieldCatalog, 6> catalogs{};
	std::copy_n(view.fields.catalogs(), catalogs.size(), catalogs.begin());
	std::array<telemetry::FieldEntry, 8> rows{};
	std::copy_n(view.fields.catalogs()[2].entries, rows.size(), rows.begin()); // batch32
	rows[0].read = +[](const void*, std::byte* bytes,
	                   telemetry::Workspace&) noexcept -> telemetry::EncodedReadResult {
		bytes[0] = std::byte{0x99};
		return {telemetry::DispatchStatus::InternalError, 1};
	};
	catalogs[2].entries = rows.data();
	view.fields = {catalogs.data(), static_cast<std::uint32_t>(catalogs.size())};
	alignas(std::max_align_t) std::array<std::byte, demo::model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 64> response{};
	const std::array<std::uint32_t, 2> selected{0x20000, 0x20001};
	const auto result = telemetry::readManyEncoded(view, selected, response, workspace);
	CHECK(result.dispatch == telemetry::DispatchStatus::Ok);
	const auto parsed = rm::parseReply(Input{response}.first(result.written), selected);
	CHECK(parsed && parsed.items[0].status == rm::ItemStatus::ReadError &&
	      parsed.items[0].payload.empty());
	CHECK(parsed.items[1].status == rm::ItemStatus::Ok &&
	      load(parsed.items[1].payload) == expected(selected[1]));
}

struct Physical {
	bool busy = false, failNext = false;
	std::span<const std::uint8_t> borrowed;
	Link* peer;

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
		for (auto byte : borrowed)
			peer->consume({&byte, 1});
		borrowed = {};
		busy = false;
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
			if (mode == Corrupt && i + 3 < bytes.size() && bytes[i] == 'T' && bytes[i + 1] == 'E' &&
			    bytes[i + 2] == 'Q' && bytes[i + 3] == '1')
				byte ^= 1;
			peer->consume({&byte, 1});
		}
		mode = Valid;
		return true;
	}
};

Input input(const Link::Packet& packet)
{
	return {reinterpret_cast<const std::byte*>(packet.data().data()), packet.data().size()};
}

struct Harness {
	Link board, host;
	Physical uart{false, false, {}, &host};
	Feeder source{Feeder::Valid, &board};
	demo::ServerLoop<Physical, Link> loop{uart, board};

	Harness()
	{
		CHECK(board.bind(tiny::bind<&Physical::send>(uart), tiny::bind<&Physical::tx_busy>(uart)));
		CHECK(host.bind(tiny::bind<&Feeder::send>(source), tiny::bind<&Feeder::busy>(source)));
		demo::status = {0, 0, 0, 0, true};
		demo::scalarFixture.calls = 0;
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
		CHECK(loop.step(1) == demo::Action::Dispatched);
		CHECK(loop.step(2) == demo::Action::Sent);
		uart.complete();
		board.poll(3);
		auto packet = host.pop_packet();
		CHECK(bool(packet));
		return packet;
	}

	void discover()
	{
		request(list());
		const auto packet = reply();
		CHECK(bool(rc::parseList(input(packet))));
	}
};

void transportOwnershipAndRecovery()
{
	Harness h;
	h.discover();
	const auto all = ids();
	for (std::size_t count : {8u, 12u, 16u, 32u}) {
		const auto selected = std::span{all}.first(count);
		h.request(batch(selected).input());
		const auto packet = h.reply();
		const auto parsed = ew::parse(input(packet));
		CHECK(parsed && parsed.header.dispatch == 0);
		const auto values = rm::parseReply(parsed.payload, selected);
		CHECK(values && values.count == count);
		for (std::size_t i = 0; i < count; ++i)
			CHECK(values.items[i].status == rm::ItemStatus::Ok &&
			      load(values.items[i].payload) == expected(selected[i]));
	}
	h.request(batch(all).input());
	CHECK(h.loop.step(4) == demo::Action::Dispatched);
	CHECK(h.loop.step(5) == demo::Action::Sent);
	h.request(list()); // New discovery cannot pass either old TX ownership gate.
	const auto before = demo::status.requests;
	const auto getters = demo::scalarFixture.calls;
	std::array<std::uint8_t, 554> preserved{};
	CHECK(h.uart.borrowed.size() <= preserved.size());
	std::copy(h.uart.borrowed.begin(), h.uart.borrowed.end(), preserved.begin());
	for (unsigned i = 0; i < 100; ++i)
		CHECK(h.loop.step(6) == demo::Action::Busy);
	CHECK(demo::status.requests == before && demo::scalarFixture.calls == getters);
	CHECK(std::equal(h.uart.borrowed.begin(), h.uart.borrowed.end(), preserved.begin()));
	h.uart.complete();
	CHECK(!h.uart.tx_busy() && h.board.tx_active());
	CHECK(h.loop.step(7) == demo::Action::Busy);
	{
		const auto packet = h.host.pop_packet();
		const auto reply = ew::parse(input(packet));
		CHECK(reply && reply.header.session == 1 && bool(rm::parseReply(reply.payload, all)));
	}
	h.board.poll(8);
	CHECK(h.loop.step(9) == demo::Action::Dispatched);
	CHECK(demo::status.requests == before + 1 && demo::scalarFixture.calls == getters);
	CHECK(h.loop.step(10) == demo::Action::Sent);
	h.uart.complete();
	h.board.poll(11);
	{
		const auto packet = h.host.pop_packet();
		CHECK(bool(rc::parseList(input(packet))));
	}
	h.request(batch(all, 1).input());
	CHECK(h.loop.step(12) == demo::Action::Refused);
	h.request(field(65536, 1).input());
	CHECK(h.loop.step(13) == demo::Action::Refused);
	CHECK(demo::scalarFixture.calls == getters);
	h.request(batch(all, 2).input());
	{
		const auto packet = h.reply();
		const auto reply = ew::parse(input(packet));
		CHECK(reply && reply.header.session == 2 && bool(rm::parseReply(reply.payload, all)));
	}
	const auto valid = batch(all, 2);
	const auto completed = demo::scalarFixture.calls;
	h.source.mode = Feeder::Corrupt;
	h.request(valid.input());
	CHECK(h.loop.step(20) == demo::Action::Idle);
	CHECK(h.board.stats().rx.crc_errors == 1 && demo::scalarFixture.calls == completed);
	h.source.mode = Feeder::Truncated;
	h.request(valid.input());
	CHECK(h.loop.step(21) == demo::Action::Idle);
	h.board.notify_gap();
	const std::uint8_t delimiter = 0;
	h.board.consume({&delimiter, 1});
	h.request(valid.input());
	CHECK(h.loop.step(22) == demo::Action::Dispatched);
	h.uart.failNext = true;
	CHECK(h.loop.step(23) == demo::Action::TxFailed);
	CHECK(h.loop.step(24) == demo::Action::Idle && !h.uart.tx_busy());
	CHECK(demo::scalarFixture.calls == completed + 31); // Never replay failed TX.
	h.discover(); // Recovery observes a complete ordered bootstrap boundary.
	h.request(batch(all, 2).input());
	CHECK(h.loop.step(25) == demo::Action::Refused);
	h.request(batch(all, 3).input());
	{
		const auto packet = h.reply();
		const auto reply = ew::parse(input(packet));
		CHECK(reply && reply.header.session == 3 && bool(rm::parseReply(reply.payload, all)));
	}
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
	descriptorAndEpochs();
	batchesAndPreflight();
	readErrorIsolated();
	transportOwnershipAndRecovery();
	std::printf("NUCLEO H7S ReadMany checks=%u passed, scalarFields=32, fixed pools, hardware=0\n",
	            checks);
}

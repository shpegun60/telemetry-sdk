/*
 * @file main.cpp
 * @brief Native stream boundaries and real Descriptor/Encoded dispatch qualification.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
#include <integration/tcp/Server.hpp>
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <resource/BytesFile.hpp>
#include <resource/protocol/Packets.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/model/Model.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

#ifdef TCP_STREAM_NO_HEAP
[[noreturn]] static void heap() noexcept
{
	std::fputs("native TCP allocation refused\n", stderr);
	std::_Exit(86);
}

void* operator new(std::size_t)
{
	heap();
}

void* operator new[](std::size_t)
{
	heap();
}

void* operator new(std::size_t, std::align_val_t)
{
	heap();
}

void* operator new[](std::size_t, std::align_val_t)
{
	heap();
}

void operator delete(void*) noexcept
{
	heap();
}

void operator delete[](void*) noexcept
{
	heap();
}

void operator delete(void*, std::size_t) noexcept
{
	heap();
}

void operator delete[](void*, std::size_t) noexcept
{
	heap();
}

void operator delete(void*, std::align_val_t) noexcept
{
	heap();
}

void operator delete[](void*, std::align_val_t) noexcept
{
	heap();
}

void operator delete(void*, std::size_t, std::align_val_t) noexcept
{
	heap();
}

void operator delete[](void*, std::size_t, std::align_val_t) noexcept
{
	heap();
}
#endif

namespace {
namespace tcp = telemetry::integration::tcp;
namespace ew = telemetry::protocol::encoded;
namespace rc = resource::protocol::client;
using resource::Input;
using resource::Output;
using resource::protocol::Op;
unsigned checks = 0;

void check(bool value, int line) noexcept
{
	++checks;
	if (!value) {
		std::fprintf(stderr, "TCP stream check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

struct Device {
	std::uint32_t value = 0x12345678;
	unsigned reads = 0, writes = 0, commands = 0;
	void (*hook)() noexcept = nullptr;

	std::uint32_t read() noexcept
	{
		++reads;
		return value;
	}

	telemetry::WriteResult write(std::uint32_t next) noexcept
	{
		++writes;
		value = next;
		return telemetry::WriteResult::Applied;
	}

	telemetry::CommandResult command() noexcept
	{
		++commands;
		if (hook)
			hook();
		return telemetry::CommandResult::Executed;
	}
};

Device device;
constexpr telemetry::FieldTable localFields{
    telemetry::field<&Device::read, &Device::write>("Value", device)};
constexpr telemetry::FieldCatalogTable fields{telemetry::group("test", localFields)};
constexpr telemetry::CommandTable localCommands{
    telemetry::command<&Device::command>("Count", device)};
constexpr telemetry::CommandCatalogTable commands{telemetry::group("test", localCommands)};
constexpr telemetry::ServiceCatalogTable services{};
constexpr telemetry::Model model{fields, commands, services};
constexpr telemetry::descriptor::Descriptor descriptor{model};
constexpr auto descriptorBytes = telemetry::descriptor::packDescriptor<descriptor>();
constexpr resource::telemetry::v3::DescriptorFile descriptorFile{descriptorBytes};
constexpr std::array<std::byte, 3> decoyBytes{std::byte{9}, std::byte{0}, std::byte{8}};
constexpr resource::BytesFile decoy{decoyBytes};
const auto files =
    resource::filesystem(resource::file("/device/decoy.bin", decoy),
                         resource::file("/telemetry/descriptor.bin", descriptorFile));

struct Fixture {
	std::array<std::byte, 128> rx{};
	std::array<std::byte, 256> tx{};
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	tcp::Server server;

	explicit Fixture(std::size_t capacity = 256)
	    : server(model.view(), workspace, files.view(), rx, Output{tx}.first(capacity))
	{}
};

Input encoded(Output out, ew::Operation operation, std::uint32_t session = 7, std::uint32_t id = 1,
              std::uint32_t response = 0, Input payload = {}) noexcept
{
	ew::Header header{};
	header.operation = operation;
	header.session = session;
	header.requestId = id;
	header.expectedResponseBytes = response;
	const auto built = ew::write(out, header, payload);
	CHECK(built);
	return Input{out}.first(built.written);
}

void splitFrames(Input frame, tcp::Mode mode = tcp::Mode::Request, Op op = Op::Read) noexcept
{
	for (std::size_t split = 0; split <= frame.size(); ++split) {
		std::array<std::byte, 256> storage{};
		tcp::FrameReader reader(storage, mode, op);
		const auto first = reader.feed(frame.first(split));
		CHECK(first.consumed == split);
		CHECK(first.status == (split == frame.size() ? tcp::Status::Ready : tcp::Status::NeedMore));
		if (!reader.ready()) {
			CHECK(!reader.frame().size());
			CHECK(!reader.expect(tcp::Mode::EncodedReply) || split == 0);
			if (split == 0)
				CHECK(reader.expect(mode, op));
			const auto rest = reader.feed(frame.subspan(split));
			CHECK(rest.status == tcp::Status::Ready && rest.consumed == frame.size() - split);
		}
		CHECK(reader.frame().size() == frame.size());
		CHECK(std::memcmp(reader.frame().data(), frame.data(), frame.size()) == 0);
		CHECK(reader.feed(frame).consumed == 0);
		reader.release();
		CHECK(reader.empty() && !reader.ready() && !reader.failed());
	}
}

void framingChecks() noexcept
{
	std::array<std::byte, 256> packet{}, reply{}, storage{};
	const auto run = [&](rc::BuildResult built) {
		CHECK(built.status == rc::BuildStatus::Ok);
		const auto request = Input{packet}.first(built.written);
		splitFrames(request);
		const auto response = resource::protocol::process(files.view(), request, reply);
		CHECK(response.written > 1);
		splitFrames(Input{reply}.first(response.written), tcp::Mode::ResourceReply,
		            static_cast<Op>(std::to_integer<unsigned>(request[0])));
	};
	run(rc::makeList(packet, 0));
	run(rc::makeStat(packet, 1));
	run(rc::makeRead(packet, 1, 0));
	const std::array<std::byte, 4> payload{std::byte{0x54}, std::byte{0}, std::byte{4},
	                                       std::byte{0xff}};
	run(rc::makeWrite(packet, 0, 0, payload, true));
	run(rc::makeSave(packet, 0, 0));
	const auto request = encoded(packet, ew::Operation::FieldWrite, 7, 1, 0, payload);
	splitFrames(request);
	const auto handled = telemetry::integration::encoded::dispatchEncoded(
	    model.view(), *([]() -> telemetry::Workspace* {
		    static std::array<std::byte, model.maxScratch()> scratch{};
		    static telemetry::Workspace ws{scratch};
		    return &ws;
	    })(),
	    7, request, reply);
	CHECK(handled);
	splitFrames(Input{reply}.first(handled.written), tcp::Mode::EncodedReply);

	const auto stat = rc::makeStat(packet, 0);
	std::copy_n(packet.begin(), stat.written, packet.begin() + stat.written);
	tcp::FrameReader reader(storage);
	const auto one = reader.feed(Input{packet}.first(stat.written * 2));
	CHECK(one.status == tcp::Status::Ready && one.consumed == stat.written);
	reader.release();
	CHECK(reader.feed(Input{packet}.subspan(one.consumed, stat.written)).status ==
	      tcp::Status::Ready);
	reader.release();
	const std::array<std::byte, 1> invalid{std::byte{9}};
	CHECK(reader.feed(invalid).status == tcp::Status::Invalid);
	CHECK(reader.failed() && reader.feed(Input{packet}.first(stat.written)).consumed == 0);
	reader.reset(tcp::Mode::ResourceReply, Op::Read);
	const std::array<std::byte, 1> shortError{std::byte(resource::Status::InvalidData)};
	CHECK(reader.feed(shortError).status == tcp::Status::NeedMore);
	CHECK(reader.partial() && !reader.ready());
	reader.close();
	CHECK(reader.failed() && reader.frame().empty());

	reader.reset();
	encoded(packet, ew::Operation::Command);
	packet[20] = packet[21] = packet[22] = packet[23] = std::byte{0xff};
	CHECK(reader.feed(Input{packet}.first(32)).status == tcp::Status::TooLarge);
	reader.reset();
	encoded(packet, ew::Operation::Command);
	packet[5] = std::byte{3};
	CHECK(reader.feed(Input{packet}.first(32)).status == tcp::Status::Invalid);
	reader.reset();
	encoded(packet, ew::Operation::Command);
	packet[1] = std::byte{0};
	CHECK(reader.feed(Input{packet}.first(32)).status == tcp::Status::Invalid);
	reader.reset();
	CHECK(rc::makeWrite(packet, 0, 0, payload, true).status == rc::BuildStatus::Ok);
	packet[13] = std::byte{2};
	CHECK(reader.feed(Input{packet}.first(16)).status == tcp::Status::Invalid);
	tcp::FrameReader tiny(Output{storage}.first(4));
	CHECK(tiny.feed(Input{packet}.first(16)).status == tcp::Status::TooLarge);

	// Normal provider errors may begin with InvalidData and still have full headers.
	reader.reset(tcp::Mode::ResourceReply, Op::Read);
	reply.fill(std::byte{0});
	reply[0] = std::byte(resource::Status::InvalidData);
	CHECK(reader.feed(Input{reply}.first(1)).status == tcp::Status::NeedMore);
	CHECK(reader.feed(Input{reply}.subspan(1, 11)).status == tcp::Status::Ready);
	CHECK(rc::parseRead(reader.frame()));
}

void inputOverlapChecks() noexcept
{
	std::array<std::byte, 32> packet{};
	const auto built = rc::makeRead(packet, 0x12345678, UINT64_C(0x1122334455667788));
	CHECK(built.status == rc::BuildStatus::Ok && built.written == 13);
	const auto request = Input{packet}.first(built.written);
	// Exact overlap, input after storage begins, and input before storage begins.
	// Refusal must precede even the first-byte store, not merely the bulk copy.
	for (const auto offset : {32u, 37u, 27u}) {
		std::array<std::byte, 96> backing{};
		backing.fill(std::byte{0xa5});
		std::copy(request.begin(), request.end(), backing.begin() + offset);
		const auto before = backing;
		const auto storage = Output{backing}.subspan(32, 32);
		tcp::FrameReader reader{storage};
		const auto rejected = reader.feed(Input{backing}.subspan(offset, request.size()));
		CHECK(rejected.status == tcp::Status::Invalid && rejected.consumed == 0);
		CHECK(backing == before && reader.empty() && !reader.partial() && !reader.ready() &&
		      !reader.failed());
		CHECK(reader.feed(Input{storage}.first(0)).status == tcp::Status::NeedMore);
		CHECK(reader.feed(request).status == tcp::Status::Ready);
		CHECK(reader.frame().size() == request.size() &&
		      std::memcmp(reader.frame().data(), request.data(), request.size()) == 0);
	}
	std::array<std::byte, 96> backing{};
	const auto storage = Output{backing}.subspan(32, 32);
	tcp::FrameReader reader{storage};
	CHECK(reader.feed(request.first(4)).status == tcp::Status::NeedMore);
	CHECK(reader.partial());
	std::copy(request.begin() + 4, request.end(), backing.begin() + 48);
	const auto before = backing;
	const auto rejected = reader.feed(Input{backing}.subspan(48, request.size() - 4));
	CHECK(rejected.status == tcp::Status::Invalid && rejected.consumed == 0);
	CHECK(backing == before && reader.partial() && !reader.empty() && !reader.ready() &&
	      !reader.failed());
	const auto completed = reader.feed(request.subspan(4));
	CHECK(completed.status == tcp::Status::Ready && completed.consumed == request.size() - 4);
	CHECK(reader.frame().size() == request.size() &&
	      std::memcmp(reader.frame().data(), request.data(), request.size()) == 0);
	// An already-ready reader keeps its existing no-consumption precedence.
	CHECK(reader.feed(reader.frame()).status == tcp::Status::Ready);
	reader.release();
	// Adjacent nonoverlapping spans in the same array are accepted.
	for (const auto offset : {32 - request.size(), std::size_t{64}}) {
		std::copy(request.begin(), request.end(), backing.begin() + offset);
		const auto result = reader.feed(Input{backing}.subspan(offset, request.size()));
		CHECK(result.status == tcp::Status::Ready && result.consumed == request.size());
		CHECK(reader.frame().size() == request.size() &&
		      std::memcmp(reader.frame().data(), request.data(), request.size()) == 0);
		reader.release();
	}
}

void serverChecks() noexcept
{
	std::array<std::byte, 512> packet{}, joined{};
	Fixture fixture;
	const auto list = rc::makeList(packet, 0);
	for (std::size_t i = 0; i < list.written; ++i) {
		const auto result = fixture.server.receive(Input{packet}.subspan(i, 1));
		CHECK(result.consumed == 1);
		CHECK(result.status ==
		      (i + 1 == list.written ? tcp::Status::Ready : tcp::Status::NeedMore));
	}
	const auto paths = rc::parseList(fixture.server.reply());
	CHECK(paths && paths.response.paths.size() == 2);
	auto path = paths.response.paths.begin();
	CHECK(*path == "/device/decoy.bin");
	++path;
	CHECK(*path == "/telemetry/descriptor.bin");
	CHECK(fixture.server.session() == 0);
	CHECK(fixture.server.receive(Input{packet}.first(list.written)).status == tcp::Status::Blocked);
	fixture.server.releaseReply();
	resource::Cursor cursor = 0;
	std::array<std::byte, descriptorBytes.size()> downloaded{};
	std::size_t copied = 0;
	for (;;) {
		const auto built = rc::makeRead(packet, 1, cursor);
		CHECK(fixture.server.receive(Input{packet}.first(built.written)).status ==
		      tcp::Status::Ready);
		const auto reply = rc::parseRead(fixture.server.reply());
		CHECK(reply && reply.response.status == resource::Status::Ok);
		CHECK(reply.response.data.size() <= downloaded.size() - copied);
		std::copy(reply.response.data.begin(), reply.response.data.end(),
		          downloaded.begin() + copied);
		copied += reply.response.data.size();
		cursor = reply.response.next;
		const auto eof = reply.response.eof;
		fixture.server.releaseReply();
		if (eof)
			break;
	}
	CHECK(copied == descriptorBytes.size() && downloaded == descriptorBytes);

	device.value = 0x12345678;
	auto request = encoded(packet, ew::Operation::FieldRead, 17, 5, 4);
	CHECK(fixture.server.receive(request).status == tcp::Status::Ready);
	const auto read = ew::parse(fixture.server.reply());
	CHECK(read && read.header.reply && read.header.session == 17 && read.header.requestId == 5);
	CHECK(read.payload.size() == 4 && read.payload[0] == std::byte{0x78});
	CHECK(fixture.server.session() == 17 && fixture.server.lastRequestId() == 5);
	fixture.server.releaseReply();
	const auto calls = device.commands;
	request = encoded(packet, ew::Operation::Command, 17, 6);
	std::copy(request.begin(), request.end(), joined.begin());
	std::copy(request.begin(), request.end(), joined.begin() + request.size());
	const auto first = fixture.server.receive(Input{joined}.first(request.size() * 2));
	CHECK(first.status == tcp::Status::Ready && first.consumed == request.size());
	CHECK(device.commands == calls + 1);
	fixture.server.releaseReply();
	CHECK(fixture.server.receive(Input{joined}.subspan(first.consumed, request.size())).status ==
	      tcp::Status::Invalid);
	CHECK(fixture.server.closed() && fixture.server.reply().empty());
	CHECK(device.commands == calls + 1);

	Fixture coalesced;
	const auto twoCalls = device.commands;
	request = encoded(packet, ew::Operation::Command, 22, 1);
	std::copy(request.begin(), request.end(), joined.begin());
	const auto secondRequest = encoded(packet, ew::Operation::Command, 22, 2);
	std::copy(secondRequest.begin(), secondRequest.end(), joined.begin() + request.size());
	const auto accepted = coalesced.server.receive(Input{joined}.first(request.size() * 2));
	CHECK(accepted.status == tcp::Status::Ready && accepted.consumed == request.size());
	CHECK(device.commands == twoCalls + 1);
	CHECK(
	    coalesced.server.receive(Input{joined}.subspan(accepted.consumed, request.size())).status ==
	    tcp::Status::Blocked);
	CHECK(device.commands == twoCalls + 1);
	coalesced.server.releaseReply();
	CHECK(
	    coalesced.server.receive(Input{joined}.subspan(accepted.consumed, request.size())).status ==
	    tcp::Status::Ready);
	CHECK(device.commands == twoCalls + 2);
	CHECK(coalesced.server.lastRequestId() == 2);

	Fixture wrongSession;
	CHECK(wrongSession.server.receive(encoded(packet, ew::Operation::Command, 18, 1)).status ==
	      tcp::Status::Ready);
	wrongSession.server.releaseReply();
	const auto prior = device.commands;
	CHECK(wrongSession.server.receive(encoded(packet, ew::Operation::Command, 19, 2)).status ==
	      tcp::Status::Invalid);
	CHECK(device.commands == prior && wrongSession.server.closed());

	Fixture small(31);
	CHECK(small.server.receive(encoded(packet, ew::Operation::Command)).status ==
	      tcp::Status::TooLarge);
	CHECK(device.commands == prior && small.server.closed());
	Fixture malformed;
	encoded(packet, ew::Operation::Command);
	packet[29] = std::byte{1};
	CHECK(malformed.server.receive(Input{packet}.first(32)).status == tcp::Status::Invalid);
	CHECK(device.commands == prior);
	Fixture badPayload;
	const auto original = device.value;
	const auto written = device.writes;
	CHECK(badPayload.server.receive(encoded(packet, ew::Operation::FieldWrite)).status ==
	      tcp::Status::Ready);
	const auto refusal = ew::parse(badPayload.server.reply());
	CHECK(refusal && refusal.header.dispatch ==
	                     static_cast<unsigned>(telemetry::DispatchStatus::InvalidPayload));
	CHECK(device.value == original && device.writes == written);
	const auto saved = badPayload.tx;
	badPayload.server.close();
	CHECK(badPayload.server.reply().empty() && badPayload.tx == saved);
	CHECK(badPayload.server.receive(Input{packet}.first(32)).status == tcp::Status::Closed);

	Fixture readSmall(32);
	const auto reads = device.reads;
	CHECK(readSmall.server.receive(encoded(packet, ew::Operation::FieldRead, 7, 1, 4)).status ==
	      tcp::Status::Ready);
	const auto capacity = ew::parse(readSmall.server.reply());
	CHECK(capacity && capacity.header.dispatch ==
	                      static_cast<unsigned>(telemetry::DispatchStatus::BufferTooSmall));
	CHECK(device.reads == reads);
}

tcp::Server* active = nullptr;
bool reentryRejected = false;

void closeHook() noexcept
{
	active->close();
}

void reentryHook() noexcept
{
	reentryRejected = active->receive({}).status == tcp::Status::Reentrant;
}

void lifetimeChecks() noexcept
{
	std::array<std::byte, 64> packet{};
	Fixture fixture;
	active = &fixture.server;
	device.hook = closeHook;
	CHECK(fixture.server.receive(encoded(packet, ew::Operation::Command)).status ==
	      tcp::Status::Closed);
	CHECK(fixture.server.closed() && fixture.server.reply().empty());
	Fixture reentry;
	active = &reentry.server;
	device.hook = reentryHook;
	CHECK(reentry.server.receive(encoded(packet, ew::Operation::Command)).status ==
	      tcp::Status::Ready);
	CHECK(reentryRejected && !reentry.server.closed());
	device.hook = nullptr;
	active = nullptr;
	std::array<std::byte, 128> aliased{};
	std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	tcp::Server invalid(model.view(), workspace, files.view(), aliased, aliased);
	CHECK(invalid.closed());
}
} // namespace

int main()
{
#if defined(TCP_STREAM_NO_HEAP) && defined(TCP_STREAM_HEAP_CONTROL)
	void* volatile allocation = ::operator new(1);
	(void)allocation;
	return 1;
#else
	framingChecks();
	inputOverlapChecks();
	serverChecks();
	lifetimeChecks();
	std::printf("Native TCP stream: %u checks, 0 failures\n", checks);
#endif
}

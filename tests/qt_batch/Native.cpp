// Named TCP Encoded profile: real Resource/Encoded boundaries, no socket. MIT.
#include <integration/tcp/Server.hpp>
#include <integration/tcp/EncodedProfile.hpp>
#include <integration/tcp/ReadManyProfile.hpp>
#include <resource/protocol/Packets.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace tcp = telemetry::integration::tcp;
namespace ew = telemetry::protocol::encoded;
namespace rm = ew::read_many;
namespace rc = resource::protocol::client;
using resource::Input;
using resource::Output;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "TCP ReadMany profile check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

struct Device {
	unsigned reads = 0;
	void (*hook)() = nullptr;

	std::uint32_t get() noexcept
	{
		++reads;
		if (hook)
			hook();
		return 0x12345678;
	}
} device;

constexpr telemetry::FieldTable localFields{telemetry::field<&Device::get>("Value", device)};
constexpr telemetry::FieldCatalogTable fields{telemetry::group("test", localFields)};
constexpr telemetry::CommandCatalogTable commands{};
constexpr telemetry::ServiceCatalogTable services{};
constexpr telemetry::Model model{fields, commands, services};
constexpr telemetry::descriptor::Descriptor descriptor{model};
constexpr auto packed = telemetry::descriptor::packDescriptor<descriptor>();
static_assert(!std::is_copy_constructible_v<tcp::EncodedProfile>);
static_assert(!std::is_move_constructible_v<tcp::EncodedProfile>);

struct Fixture {
	tcp::EncodedProfile profile{packed, descriptor.fingerprint()};
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 1024> rx{}, tx{};
	tcp::Server server;

	explicit Fixture(std::size_t replyCapacity = 1024)
	    : server(model.view(), workspace, profile, rx, Output{tx}.first(replyCapacity))
	{}
};

std::array<std::byte, 4> sessionBytes(std::uint32_t value)
{
	return {std::byte(value & 255), std::byte((value >> 8) & 255), std::byte((value >> 16) & 255),
	        std::byte(value >> 24)};
}

resource::protocol::Reply write(tcp::EncodedProfile& profile, std::uint32_t session,
                                resource::Cursor cursor = 0, bool final = true,
                                std::size_t payloadBytes = 4, std::size_t replyBytes = 64)
{
	std::array<std::byte, 64> request{}, response{};
	std::array<std::byte, 5> input{};
	const auto bytes = sessionBytes(session);
	std::copy(bytes.begin(), bytes.end(), input.begin());
	const auto built = rc::makeWrite(request, 1, cursor, Input{input}.first(payloadBytes), final);
	CHECK(built.status == rc::BuildStatus::Ok);
	const auto result = resource::protocol::process(
	    profile.files(), Input{request}.first(built.written), Output{response}.first(replyBytes));
	if (result.written) {
		const auto parsed = rc::parseWrite(Input{response}.first(result.written), payloadBytes);
		CHECK(parsed && !parsed.shortError);
		CHECK(parsed.response.status == result.status);
		if (result.status != resource::Status::Ok)
			CHECK(parsed.response.consumed == 0 && parsed.response.next == cursor &&
			      !parsed.response.complete);
	}
	return result;
}

void bind(Fixture& fixture, std::uint32_t session = 7)
{
	CHECK(write(fixture.profile, session).status == resource::Status::Ok);
	CHECK(fixture.profile.bound() && !fixture.profile.frozen());
	CHECK(fixture.profile.readManyCapability().session == session);
	CHECK(fixture.profile.readManyCapability().fingerprint == descriptor.fingerprint());
}

Input readMany(Output output, std::uint32_t session = 7, std::uint32_t requestId = 1)
{
	const std::array<std::uint32_t, 1> ids{0};
	const auto body = rm::writeRequest(output.subspan(32), ids);
	CHECK(body);
	ew::Header header{};
	header.operation = ew::Operation::ReadMany;
	header.session = session;
	header.requestId = requestId;
	header.payloadBytes = static_cast<std::uint32_t>(body.written);
	header.expectedResponseBytes = 514;
	CHECK(ew::writeHeader(output, header) == ew::Status::Ok);
	return Input{output}.first(32 + body.written);
}

Input batchRead(Output output, std::uint32_t session = 7, std::uint32_t requestId = 1)
{
	const std::array<ew::batch::RequestItem, 1> items{{{ew::Operation::FieldRead, 0, {}}}};
	const auto body = ew::batch::writeRequest(output.subspan(32), items);
	CHECK(body);
	ew::Header header{};
	header.operation = ew::Operation::Batch;
	header.session = session;
	header.requestId = requestId;
	header.payloadBytes = static_cast<std::uint32_t>(body.written);
	header.expectedResponseBytes = 18;
	CHECK(ew::writeHeader(output, header) == ew::Status::Ok);
	return Input{output}.first(32 + body.written);
}

void batchChecks()
{
	Fixture f;
	bind(f);
	CHECK(f.profile.batchCapability().session == f.profile.readManyCapability().session);
	std::array<std::byte, 24> capability{};
	const auto read = f.profile.files().read(3, 0, capability);
	CHECK(read.status == resource::Status::Ok && read.written == 24 && read.eof);
	const auto parsed = ew::batch::parseCapability(capability);
	CHECK(parsed && parsed.value == f.profile.batchCapability());
	const auto before = device.reads;
	std::array<std::byte, 256> request{};
	CHECK(f.server.receive(batchRead(request)).status == tcp::Status::Ready);
	CHECK(device.reads == before + 1 && f.profile.frozen());
	const std::array<ew::batch::RequestItem, 1> expected{{{ew::Operation::FieldRead, 0, {}}}};
	const auto reply = ew::batch::parseReply(f.server.reply().subspan(32), expected);
	CHECK(reply && reply.count == 1 && reply.items[0].payload.size() == 4);
	f.server.releaseReply();
	CHECK(f.server.receive(readMany(request, 7, 2)).status == tcp::Status::Ready);
	CHECK(device.reads == before + 2); // Both op5 and op6 share the one session.
}

void profileChecks()
{
	Fixture f;
	const auto before = device.reads;
	CHECK(!f.profile.bound() && !f.profile.frozen());
	CHECK(f.profile.files().fileCount() == 4);
	CHECK(f.profile.files().stat(0).flags == resource::FileFlag::Readable);
	CHECK(f.profile.files().stat(1).flags == resource::FileFlag::Writable);
	CHECK(f.profile.files().stat(2).flags == resource::FileFlag::Readable);
	std::array<std::byte, 24> unopened{};
	unopened.fill(std::byte{0x5a});
	const auto unopenedRead = f.profile.files().read(2, 0, unopened);
	CHECK(unopenedRead.status != resource::Status::Ok && unopenedRead.written == 0);
	CHECK(std::all_of(unopened.begin(), unopened.end(), [](std::byte value) {
		return value == std::byte{0x5a};
	}));
	CHECK(!f.profile.bound());
	for (const auto size : {std::size_t{0}, std::size_t{3}, std::size_t{5}}) {
		CHECK(write(f.profile, 7, 0, true, size).status != resource::Status::Ok);
		CHECK(!f.profile.bound());
	}
	CHECK(write(f.profile, 0).status != resource::Status::Ok);
	CHECK(write(f.profile, 7, 1).status != resource::Status::Ok);
	CHECK(write(f.profile, 7, 0, false).status != resource::Status::Ok);
	CHECK(!f.profile.bound());
	CHECK(write(f.profile, 7, 0, true, 4, 13).status == resource::Status::BufferTooSmall);
	CHECK(!f.profile.bound()); // ACK capacity is checked before session-control side effects.
	bind(f);
	for (const auto session : {7u, 8u, UINT32_MAX}) {
		CHECK(write(f.profile, session).status != resource::Status::Ok);
		CHECK(f.profile.readManyCapability().session == 7);
	}
	std::array<std::byte, 24> capability{};
	const auto read = f.profile.files().read(2, 0, capability);
	CHECK(read.status == resource::Status::Ok && read.written == 24 && read.eof);
	const auto parsed = rm::parseCapability(capability);
	CHECK(parsed && parsed.value == f.profile.readManyCapability());
	std::array<std::byte, 23> first{};
	std::array<std::byte, 1> last{};
	const auto prefix = f.profile.files().read(2, 0, first);
	const auto suffix = f.profile.files().read(2, prefix.next, last);
	CHECK(prefix.status == resource::Status::Ok && prefix.written == 23 && !prefix.eof);
	CHECK(suffix.status == resource::Status::Ok && suffix.written == 1 && suffix.eof);
	CHECK(std::equal(first.begin(), first.end(), capability.begin()) && last[0] == capability[23]);
	CHECK(!f.profile.frozen() && device.reads == before);
	telemetry::Workspace overlapping{{reinterpret_cast<std::byte*>(&f.profile), sizeof(f.profile)}};
	tcp::Server invalid(model.view(), overlapping, f.profile, f.rx, f.tx);
	CHECK(invalid.closed() && f.profile.readManyCapability().session == 7);
	// Invalid configured limits cannot publish a bound session or capability.
	tcp::EncodedProfile invalidLimits{packed, descriptor.fingerprint(), 0};
	CHECK(write(invalidLimits, 7).status != resource::Status::Ok && !invalidLimits.bound());
}

Fixture* active = nullptr;

void duringGetter()
{
	CHECK(active && active->profile.frozen());
	CHECK(active->server.receive({}).status == tcp::Status::Reentrant);
	CHECK(write(active->profile, 8).status != resource::Status::Ok);
}

void dispatchChecks()
{
	const auto before = device.reads;
	std::array<std::byte, 256> request{};
	{
		Fixture f;
		const auto frame = readMany(request);
		CHECK(f.server.receive(frame).status != tcp::Status::Ready);
		CHECK(device.reads == before && f.server.closed());
	}
	{
		Fixture f;
		bind(f);
		CHECK(f.server.receive(readMany(request, 8)).status != tcp::Status::Ready);
		CHECK(device.reads == before && f.server.closed());
	}
	{
		Fixture f(31);
		bind(f);
		CHECK(f.server.receive(readMany(request)).status != tcp::Status::Ready);
		CHECK(device.reads == before);
	}
	{
		Fixture f;
		bind(f);
		active = &f;
		device.hook = &duringGetter;
		CHECK(f.server.receive(readMany(request)).status == tcp::Status::Ready);
		device.hook = nullptr;
		active = nullptr;
		CHECK(f.profile.frozen() && f.server.session() == 7 && f.server.lastRequestId() == 1);
		CHECK(device.reads == before + 1);
		const auto reply = ew::parse(f.server.reply());
		CHECK(reply && reply.header.reply && reply.header.session == 7);
		const std::array<std::uint32_t, 1> ids{0};
		const auto values = rm::parseReply(f.server.reply().subspan(32), ids);
		CHECK(values && values.count == 1 && values.items[0].status == rm::ItemStatus::Ok);
		CHECK(values.items[0].payload.size() == 4 && values.items[0].payload[0] == std::byte{0x78});
		CHECK(write(f.profile, 7).status != resource::Status::Ok);
		CHECK(write(f.profile, 8).status != resource::Status::Ok);
		f.server.releaseReply();
		CHECK(f.server.receive(readMany(request, 7, 1)).status != tcp::Status::Ready);
		CHECK(device.reads == before + 1); // Replayed request never reaches the getter.
		f.server.close();
		CHECK(write(f.profile, 9).status != resource::Status::Ok); // Close never resets identity.
	}
}
} // namespace

int main()
{
	profileChecks();
	dispatchChecks();
	batchChecks();
	std::printf("TCP Encoded profile: %u checks, 0 failures\n", checks);
}

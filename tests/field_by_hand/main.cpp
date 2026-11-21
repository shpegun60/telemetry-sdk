/*
 * @file main.cpp
 * @brief The bytes of "Reading a Field without the SDK", produced by the real codec and server.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The group_tree example's Model runs behind the TCP server and the encoded
 * profile a device uses. Each request is built with the SDK's own builders,
 * fed to the server as a client would send it, and printed with the server's
 * reply as BLOCK <name> ... END. run.py compares every block with the hex in
 * doc/user/ReadingAFieldByHand.md, so the walkthrough cannot drift from the
 * code. No socket, thread or hardware is involved.
 */

#include "../../examples/group_tree/Telemetry.hpp"

#include <integration/tcp/EncodedProfile.hpp>
#include <integration/tcp/Server.hpp>
#include <resource/protocol/Packets.hpp>
#include <telemetry/codec/Codec.hpp>
#include <wire/EncodedWire.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

namespace {
namespace ts = telemetry;
namespace tcp = telemetry::integration::tcp;
namespace wire = telemetry::protocol::encoded;
namespace client = resource::protocol::client;
using Bytes = std::span<const std::byte>;

unsigned checks = 0, failures = 0;

void check(bool condition, const char* what)
{
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAILED: %s\n", what);
	}
}

void block(const char* name, Bytes bytes)
{
	std::printf("BLOCK %s\n", name);
	for (std::size_t i = 0; i < bytes.size(); ++i)
		std::printf("%02x%c", std::to_integer<unsigned>(bytes[i]),
		            i + 1 == bytes.size() || i % 16 == 15 ? '\n' : ' ');
	std::printf("END\n");
}

std::uint32_t load32(Bytes bytes, std::size_t offset)
{
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
	return value;
}

inline constexpr auto packed = ts::descriptor::packDescriptor<device::descriptor>();
inline constexpr auto gainId = ts::idOf<device::model, "adc/channel1/Gain">();
static_assert(gainId == device::gain1.id && gainId == 0x00010000u, "the walkthrough's 1:0");

std::array<std::byte, 256> scratch{};
ts::Workspace workspace{scratch};
std::array<std::byte, 2048> rx{}, tx{};

// Feeds one complete request to the server, prints it and the reply, and
// returns the reply, borrowed until the next exchange.
Bytes exchange(tcp::Server& server, const char* name, Bytes request)
{
	static std::array<std::byte, 2048> reply{};
	static std::size_t replied = 0;
	char label[64];
	std::snprintf(label, sizeof label, "%s-request", name);
	block(label, request);
	const auto fed = server.receive(request);
	check(fed.status == tcp::Status::Ready && fed.consumed == request.size(), "a whole reply");
	const auto out = server.reply();
	replied = out.size();
	std::memcpy(reply.data(), out.data(), replied);
	server.releaseReply();
	std::snprintf(label, sizeof label, "%s-reply", name);
	block(label, Bytes{reply}.first(replied));
	return Bytes{reply}.first(replied);
}

// The descriptor record of the nth Field: records follow the header's
// endpointsOffset in canonical order, Fields first.
Bytes fieldRecord(std::size_t nth)
{
	const Bytes bytes{packed};
	std::size_t at = load32(bytes, 60);
	for (std::size_t i = 0; i < nth; ++i)
		at += 8 + load32(bytes, at + 4);
	check(bytes[at] == std::byte{3}, "a Field record");
	return bytes.subspan(at, 8 + load32(bytes, at + 4));
}
} // namespace

int main()
{
	tcp::EncodedProfile profile(Bytes{packed}, device::descriptor.fingerprint());
	tcp::Server server(device::model.view(), workspace, profile, rx, tx);
	check(!server.closed(), "the server opens");

	// 1. LIST from cursor 0: the profile's files, with their indices.
	std::array<std::byte, 64> request{};
	const auto list = client::makeList(request, 0);
	check(list.status == client::BuildStatus::Ok, "LIST built");
	const auto listed =
	    client::parseList(exchange(server, "list", Bytes{request}.first(list.written)));
	check(listed && listed.response.status == resource::Status::Ok && listed.response.eof,
	      "LIST answered every file at once");

	// 2. READ the descriptor (index 0) from cursor 0: it fits one reply here.
	const auto read = client::makeRead(request, 0, 0);
	check(read.status == client::BuildStatus::Ok, "READ built");
	const auto readReply = exchange(server, "read", Bytes{request}.first(read.written));
	const auto parsed = client::parseRead(readReply);
	check(parsed && parsed.response.status == resource::Status::Ok && parsed.response.eof &&
	          parsed.response.data.size() == packed.size() &&
	          std::memcmp(parsed.response.data.data(), packed.data(), packed.size()) == 0,
	      "READ returned the whole descriptor");
	block("read-reply-envelope", readReply.first(12));
	block("descriptor-header", Bytes{packed}.first(64));
	std::printf("FINGERPRINT 0x%016llx\n",
	            static_cast<unsigned long long>(device::descriptor.fingerprint()));
	std::printf("DESCRIPTOR_BYTES %zu\n", packed.size());

	// 3. The record a client takes the ID 1:0 from: the third Field, adc/channel1/Gain.
	const auto gain = fieldRecord(2);
	check(load32(gain, 8) == gainId, "the record carries the PackedId");
	block("gain-record", gain);

	// 4. Bind the session: four bytes into /telemetry/session.bin (index 1).
	const std::array<std::byte, 4> session{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
	const auto bind = client::makeWrite(request, 1, 0, session, true);
	check(bind.status == client::BuildStatus::Ok, "WRITE built");
	const auto bound = client::parseWrite(
	    exchange(server, "session", Bytes{request}.first(bind.written)), session.size());
	check(bound && bound.response.status == resource::Status::Ok && bound.response.complete,
	      "the session is bound");

	// 5. FieldRead of adc/channel1/Gain: session 1, request 1, expecting an f32.
	wire::Header header{};
	header.operation = wire::Operation::FieldRead;
	header.session = 1;
	header.requestId = 1;
	header.endpoint = gainId;
	header.expectedResponseBytes = ts::wireSize<float>;
	const auto built = wire::write(request, header);
	check(built.status == wire::Status::Ok && built.written == wire::headerBytes, "TEQ1 built");
	const auto reply = exchange(server, "field-read", Bytes{request}.first(built.written));
	const auto answer = wire::parse(reply);
	check(answer && answer.header.reply && answer.header.dispatch == 0 &&
	          answer.header.requestId == 1 && answer.payload.size() == 4,
	      "the reply parses");
	float value = 0.0f;
	check(answer && ts::decode(answer.payload, value) == ts::CodecStatus::Ok, "the f32 decodes");
	check(device::gain1.read() == value, "the device's own value");
	std::printf("VALUE %g\n", static_cast<double>(value));

	// The value encoding of a struct: adc::Config{1000, Triggered, true}.
	std::array<std::byte, ts::wireSize<adc::Config>> config{};
	check(ts::encode(adc::Config{1000, adc::Mode::Triggered, true}, config) == ts::CodecStatus::Ok,
	      "adc::Config encodes");
	block("config-value", config);

	std::printf("Field by hand: %u checks, %u failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}

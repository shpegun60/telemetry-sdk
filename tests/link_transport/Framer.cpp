// The TCP profile framer through the medium-neutral Framer interface, without
// Qt: encoding, split and joined frames, boundaries, latching and reset. MIT.
#include <integration/tcp/ProfileFramer.hpp>
#include <wire/EncodedWire.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace lk = telemetry::integration::link;
namespace ew = telemetry::protocol::encoded;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Framer check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

std::vector<std::byte> reply(std::uint32_t requestId, std::size_t payloadBytes)
{
	std::vector<std::byte> payload(payloadBytes);
	for (std::size_t index = 0; index < payload.size(); ++index)
		payload[index] = std::byte(index * 7 + requestId);
	ew::Header header;
	header.reply = true;
	header.operation = ew::Operation::FieldRead;
	header.session = 3;
	header.requestId = requestId;
	header.expectedResponseBytes =
	    static_cast<std::uint32_t>(payloadBytes); // A read reply carries it whole.
	std::vector<std::byte> frame(ew::headerBytes + payloadBytes);
	const auto built = ew::write(frame, header, payload);
	CHECK(built && built.written == frame.size());
	return frame;
}

bool same(lk::Input left, const std::vector<std::byte>& right)
{
	return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

void encoding(lk::Framer& framer)
{
	CHECK(framer.maxFrameBytes() == telemetry::integration::tcp::ProfileFramer::frameBytes);
	const std::vector<std::byte> empty;
	CHECK(framer.encodedBytes(empty) == 0);
	const std::vector<std::byte> largest(framer.maxFrameBytes(), std::byte{0x11});
	const std::vector<std::byte> oversized(framer.maxFrameBytes() + 1, std::byte{0x11});
	CHECK(framer.encodedBytes(largest) == largest.size());
	CHECK(framer.encodedBytes(oversized) == 0);
	const auto packet = reply(1, 5);
	std::vector<std::byte> out(framer.encodedBytes(packet));
	CHECK(out.size() == packet.size() && framer.encode(packet, out) &&
	      out == packet); // No wire bytes added.
	std::vector<std::byte> shortOut(packet.size() - 1);
	CHECK(!framer.encode(packet, shortOut));
	CHECK(!framer.encode(empty, {}));
}

void frames(lk::Framer& framer)
{
	framer.reset();
	CHECK(framer.expect(lk::Expect::EncodedReply));
	const auto first = reply(7, 4), second = reply(8, 0);
	// One byte at a time: NeedMore until the last byte, which completes it.
	for (std::size_t index = 0; index + 1 < first.size(); ++index) {
		const auto fed = framer.feed(lk::Input{first}.subspan(index, 1));
		CHECK(fed.status == lk::Status::NeedMore && fed.consumed == 1);
	}
	CHECK(!framer.expect(lk::Expect::EncodedReply)); // Not at a boundary mid-frame.
	auto fed = framer.feed(lk::Input{first}.last(1));
	CHECK(fed.status == lk::Status::Ready && fed.consumed == 1 && same(framer.frame(), first));
	framer.release();
	// Two frames joined: the first feed stops exactly at the first frame's end.
	std::vector<std::byte> joined(first);
	joined.insert(joined.end(), second.begin(), second.end());
	fed = framer.feed(joined);
	CHECK(fed.status == lk::Status::Ready && fed.consumed == first.size() &&
	      same(framer.frame(), first));
	framer.release();
	fed = framer.feed(lk::Input{joined}.subspan(first.size()));
	CHECK(fed.status == lk::Status::Ready && fed.consumed == second.size() &&
	      same(framer.frame(), second));
	framer.release();
	CHECK(framer.expect(lk::Expect::ResourceReply, resource::protocol::Op::List)); // At a boundary.
}

void failures(lk::Framer& framer)
{
	// Bytes that are not a frame latch Invalid until a new connection boundary.
	framer.reset();
	CHECK(framer.expect(lk::Expect::EncodedReply));
	const std::vector<std::byte> garbage(48, std::byte{0x5a});
	CHECK(framer.feed(garbage).status == lk::Status::Invalid);
	const auto good = reply(9, 2);
	CHECK(framer.feed(good).status == lk::Status::Invalid);
	CHECK(!framer.expect(lk::Expect::EncodedReply));
	framer.reset();
	CHECK(framer.expect(lk::Expect::EncodedReply));
	CHECK(framer.feed(good).status == lk::Status::Ready && same(framer.frame(), good));
	framer.release();
	// A header announcing more than the framer holds is refused from the header.
	ew::Header header;
	header.reply = true;
	header.session = 3;
	header.requestId = 10;
	header.payloadBytes = static_cast<std::uint32_t>(framer.maxFrameBytes());
	header.expectedResponseBytes = header.payloadBytes;
	std::array<std::byte, ew::headerBytes> large{};
	CHECK(ew::writeHeader(large, header) == ew::Status::Ok);
	CHECK(framer.feed(large).status == lk::Status::Invalid);
	// close() latches as well, and reset() opens the next connection.
	framer.reset();
	CHECK(framer.expect(lk::Expect::EncodedReply));
	framer.close();
	CHECK(framer.feed(good).status == lk::Status::Invalid);
	framer.reset();
	CHECK(framer.expect(lk::Expect::EncodedReply) && framer.feed(good).status == lk::Status::Ready);
}
} // namespace

int main()
{
	telemetry::integration::tcp::ProfileFramer framer;
	encoding(framer);
	frames(framer);
	failures(framer);
	std::printf("Link framer: %u checks, 0 failures\n", checks);
	return 0;
}

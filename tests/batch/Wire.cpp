/* Mixed Batch byte contract, atomic builders and malformed-body controls. MIT. */
#include <wire/BatchWire.hpp>
#include <wire/ReadManyWire.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace ew = telemetry::protocol::encoded;
namespace bw = ew::batch;
using Input = bw::Input;
static unsigned checks;
#define CHECK(x)                                                                                   \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(x)) {                                                                                \
			std::fprintf(stderr, "Batch Wire line %d check %u\n", __LINE__, checks);               \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

static void capabilities()
{
	const bw::Capability capability{0x12345678u, UINT64_C(0x8877665544332211)};
	std::array<std::byte, 25> storage{};
	auto output = std::span{storage}.subspan(1);
	CHECK(bool(bw::writeCapability(output, capability)));
	constexpr std::array<unsigned, 24> golden{0x54, 0x42, 0x4d, 0x31, 1,    32,   1,    1,
	                                          255,  255,  255,  255,  0x78, 0x56, 0x34, 0x12,
	                                          0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
	for (std::size_t i = 0; i < golden.size(); ++i)
		CHECK(std::to_integer<unsigned>(output[i]) == golden[i]);
	CHECK(bw::parseCapability(output).value == capability);
	CHECK(!ew::read_many::parseCapability(output));
	for (std::size_t n = 0; n < 24; ++n) {
		const auto before = storage;
		CHECK(!bw::parseCapability(Input{output}.first(n)));
		CHECK(!bw::writeCapability(std::span{storage}.first(n), capability));
		CHECK(storage == before);
	}
	for (std::size_t offset : {0u, 4u, 5u, 6u, 7u, 8u, 10u, 12u}) {
		auto copy = storage;
		if (offset == 8 || offset == 10 || offset == 12) {
			const std::size_t width = offset == 12 ? 4 : 2;
			std::fill_n(copy.begin() + 1 + offset, width, std::byte{0});
		} else
			copy[1 + offset] = std::byte{0};
		CHECK(!bw::parseCapability(Input{copy}.subspan(1)));
	}
	for (const auto invalid : {bw::Capability{}, bw::Capability{1, 0, 0}, bw::Capability{1, 0, 33},
	                           bw::Capability{1, 0, 32, 9}, bw::Capability{1, 0, 32, 10, 13}}) {
		const auto before = storage;
		CHECK(!bw::writeCapability(output, invalid));
		CHECK(storage == before);
	}
}

static void readingBodies(const std::array<bw::RequestItem, 32>& mixed);

static void requests()
{
	std::array<std::byte, 4> value{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
	std::array<bw::RequestItem, 32> items{};
	for (std::size_t i = 0; i < items.size(); ++i) {
		const auto op = static_cast<ew::Operation>(1 + i % 4);
		items[i] = {op, 0x11223344u, op == ew::Operation::FieldRead ? Input{} : Input{value}};
	}
	std::array<std::byte, 512> bytes{};
	for (std::size_t n = 1; n <= items.size(); ++n) {
		const auto selected = std::span{items}.first(n);
		const auto written = bw::writeRequest(bytes, selected);
		CHECK(bool(written));
		const auto input = Input{bytes}.first(written.written);
		const auto result = bw::parseRequest(input);
		CHECK(bool(result) && result.count == n);
		for (std::size_t i = 0; i < n; ++i) {
			CHECK(result.items[i].id == items[i].id &&
			      result.items[i].operation == items[i].operation);
			CHECK(std::equal(result.items[i].payload.begin(), result.items[i].payload.end(),
			                 items[i].payload.begin(), items[i].payload.end()));
		}
		for (std::size_t size = 0; size < written.written; ++size) {
			const auto bad = bw::parseRequest(input.first(size));
			CHECK(!bad && bad.count == 0 && bad.items[0].payload.empty());
		}
		CHECK(!bw::parseRequest(Input{bytes}.first(written.written + 1)));
		const auto before = bytes;
		CHECK(!bw::writeRequest(std::span{bytes}.first(written.written - 1), selected));
		CHECK(bytes == before);
	}
	CHECK(!bw::writeRequest(bytes, {}));
	CHECK(!bw::writeRequest(bytes, items, 31));
	for (auto op : {ew::Operation::ReadMany, ew::Operation::Batch, static_cast<ew::Operation>(0),
	                static_cast<ew::Operation>(255)}) {
		const bw::RequestItem invalid{op, 1, {}};
		const auto before = bytes;
		CHECK(!bw::writeRequest(bytes, std::span{&invalid, 1}));
		CHECK(bytes == before);
	}
	const bw::RequestItem alias{ew::Operation::Command, 1, Input{bytes}.first(1)};
	const auto before = bytes;
	CHECK(!bw::writeRequest(bytes, std::span{&alias, 1}));
	CHECK(bytes == before);
	static std::array<std::byte, 65536> large{};
	const bw::RequestItem oversized{ew::Operation::Service, 1, large};
	CHECK(!bw::writeRequest(bytes, std::span{&oversized, 1}));
	const bw::RequestItem overBudget{ew::Operation::Command, 1, Input{large}.first(65530)};
	CHECK(!bw::writeRequest(bytes, std::span{&overBudget, 1}));
	CHECK(bytes == before);
	CHECK(!bw::parseRequest(large));
	CHECK(bw::hasMutations(items));
	const bw::RequestItem read{ew::Operation::FieldRead, 1, {}};
	CHECK(!bw::hasMutations(std::span{&read, 1}));
	readingBodies(items);
}

// Whether a body only reads, without parsing it into items: true exactly when
// parseRequest accepts it and every item is a FieldRead.
static void readingBodies(const std::array<bw::RequestItem, 32>& mixed)
{
	std::array<bw::RequestItem, 32> reads{};
	for (std::size_t i = 0; i < reads.size(); ++i)
		reads[i] = {ew::Operation::FieldRead, 0x01020304u + std::uint32_t(i), {}};
	std::array<std::byte, 512> bytes{};
	for (std::size_t n = 1; n <= reads.size(); ++n) {
		const auto written = bw::writeRequest(bytes, std::span{reads}.first(n));
		CHECK(bool(written) && written.written == 2 + 8 * n);
		const auto input = Input{bytes}.first(written.written);
		CHECK(bw::onlyReads(input) && bw::onlyReads(input, n) && !bw::onlyReads(input, n - 1));
		CHECK(!bw::onlyReads(input, 0) && !bw::onlyReads(input, bw::maxItems + 1));
		for (std::size_t size = 0; size < written.written; ++size)
			CHECK(!bw::onlyReads(input.first(size)));
		CHECK(!bw::onlyReads(Input{bytes}.first(written.written + 1)));
		// Each item's operation, reserved byte and length, one at a time.
		for (std::size_t item = 0; item < n; ++item) {
			const auto at = 2 + 8 * item;
			for (const auto& [offset, value] :
			     {std::pair{std::size_t{4}, std::byte{1}}, std::pair{std::size_t{4}, std::byte{5}},
			      std::pair{std::size_t{5}, std::byte{1}}, std::pair{std::size_t{6}, std::byte{1}},
			      std::pair{std::size_t{7}, std::byte{1}}}) {
				const auto kept = bytes[at + offset];
				bytes[at + offset] = value;
				CHECK(!bw::onlyReads(input));
				bytes[at + offset] = kept;
			}
		}
		CHECK(bw::onlyReads(input));
	}
	// A count of zero, and one past the items written.
	std::array<std::byte, 2> none{};
	CHECK(!bw::onlyReads(none));
	const auto one = bw::writeRequest(bytes, std::span{reads}.first(1));
	CHECK(bool(one));
	bytes[0] = std::byte{2};
	CHECK(!bw::onlyReads(Input{bytes}.first(one.written)));
	// A write, a Command or a Service anywhere: not only reads.
	for (std::size_t n = 1; n <= mixed.size(); ++n) {
		const auto written = bw::writeRequest(bytes, std::span{mixed}.first(n));
		CHECK(bool(written));
		CHECK(bw::onlyReads(Input{bytes}.first(written.written)) ==
		      !bw::hasMutations(std::span{mixed}.first(n)));
	}
	static std::array<std::byte, 65536> large{};
	CHECK(!bw::onlyReads(large));
}

static void replies()
{
	const std::array<bw::RequestItem, 2> expected{
	    {{ew::Operation::Command, 7, {}}, {ew::Operation::FieldRead, 7, {}}}};
	std::array<std::byte, 31> body{};
	bw::detail::store(body, 0, std::uint16_t{2});
	bw::detail::store(body, 2, std::uint32_t{7});
	body[6] = std::byte{2};
	body[8] = std::byte{1}; // Command Accepted.
	bw::detail::store(body, 14, std::uint32_t{7});
	body[18] = std::byte{4};
	bw::detail::store(body, 22, std::uint16_t{4});
	body[26] = std::byte{0x44};
	body[27] = std::byte{0x33};
	body[28] = std::byte{0x22};
	body[29] = std::byte{0x11};
	const auto input = Input{body}.first(30);
	const auto parsed = bw::parseReply(input, expected);
	CHECK(bool(parsed) && parsed.count == 2 && parsed.items[0].endpointStatus == 1);
	CHECK(parsed.items[0].payload.empty() && parsed.items[1].payload.size() == 4);
	CHECK(parsed.items[1].payload.data() == body.data() + 26);
	for (std::size_t n = 0; n < body.size(); ++n)
		if (n != 30) {
			const auto bad = bw::parseReply(Input{body}.first(n), expected);
			CHECK(!bad && bad.count == 0 && bad.items[0].payload.empty());
		}
	for (std::size_t offset :
	     {0u, 2u, 6u, 7u, 8u, 9u, 10u, 12u, 13u, 14u, 18u, 19u, 20u, 21u, 22u, 24u, 25u}) {
		auto changed = body;
		changed[offset] = std::byte{255};
		const auto bad = bw::parseReply(Input{changed}.first(30), expected);
		CHECK(!bad && bad.count == 0 && bad.items[1].payload.empty());
	}
	// A later malformed record must not leak an earlier successful value view.
	std::uint32_t random = 0x6d697865u;
	std::array<std::byte, 256> fuzz{};
	for (unsigned trial = 0; trial < 4000; ++trial) {
		for (auto& b : fuzz) {
			random = random * 1664525u + 1013904223u;
			b = std::byte(random >> 24);
		}
		const auto span = Input{fuzz}.first(random % fuzz.size());
		const auto req = bw::parseRequest(span);
		const auto rep = bw::parseReply(span, expected);
		if (!req)
			CHECK(req.count == 0 && req.items[0].payload.empty());
		if (!rep)
			CHECK(rep.count == 0 && rep.items[0].payload.empty());
	}
	ew::Header header{};
	header.operation = ew::Operation::Batch;
	header.session = 1;
	header.requestId = 9;
	header.expectedResponseBytes = 30;
	header.reply = true;
	std::array<std::byte, 128> frame{};
	const auto built = ew::write(frame, header, input);
	CHECK(bool(built) && bool(ew::parse(Input{frame}.first(built.written))));
	for (auto capacity : {0u, 13u, 65536u}) {
		auto invalid = header;
		invalid.expectedResponseBytes = capacity;
		CHECK(ew::validate(invalid) != ew::Status::Ok);
	}
	header.endpoint = 1;
	CHECK(ew::validate(header) != ew::Status::Ok);
	header.endpoint = 0;
	header.endpointStatus = 1;
	CHECK(ew::validate(header) != ew::Status::Ok);
}

int main()
{
	capabilities();
	requests();
	replies();
	std::printf("Batch Wire checks=%u failures=0\n", checks);
}

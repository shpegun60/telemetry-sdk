// Checked resource requests/replies, byte goldens and malformed-input controls.
// Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
#include <resource/BytesFile.hpp>
#include <resource/FileSystem.hpp>
#include <resource/protocol/Packets.hpp>
#include <resource/protocol/Protocol.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <vector>

namespace client = resource::protocol::client;
namespace wire = resource::protocol::wire;
using namespace resource;
using namespace std::literals::string_view_literals;
static unsigned checks = 0;
#define CHECK(...)                                                                                 \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(__VA_ARGS__)) {                                                                      \
			std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__);                         \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

static_assert(std::ranges::input_range<client::ListPaths>);
static_assert(wire::listRequestSize == 9 && wire::statRequestSize == 5 &&
              wire::readRequestSize == 13 && wire::writeRequestHeaderSize == 16 &&
              wire::chunkReplyHeaderSize == 12 && wire::statReplySize == 6 &&
              wire::writeReplySize == 14 && wire::maxPayloadSize == 65535 &&
              wire::maxListPathSize == 65533);
static_assert([] {
	std::array<std::byte, wire::readRequestSize> bytes{};
	const auto result = client::makeRead(bytes, 0x12345678, 0x0807060504030201);
	return result.status == client::BuildStatus::Ok && result.written == bytes.size() &&
	       bytes[0] == std::byte{3} && bytes[1] == std::byte{0x78} && bytes[12] == std::byte{8};
}());

template<std::size_t N>
static auto packet(const std::array<unsigned char, N>& golden)
{
	std::array<std::byte, N> bytes{};
	for (std::size_t i = 0; i != N; ++i) {
		bytes[i] = std::byte(golden[i]);
	}
	return bytes;
}

template<std::size_t N>
static bool equals(Input bytes, const std::array<unsigned char, N>& golden)
{
	return bytes.size() == N &&
	       std::equal(bytes.begin(), bytes.end(), golden.begin(), [](std::byte a, unsigned char b) {
		       return std::to_integer<unsigned>(a) == b;
	       });
}

template<class Parser>
static void truncatedAndTrailing(Input good, Parser parse)
{
	for (std::size_t length = 0; length < good.size(); ++length) {
		CHECK(!parse(good.first(length)));
	}
	std::vector<std::byte> extra(good.begin(), good.end());
	extra.push_back(std::byte{});
	CHECK(!parse(extra));
}

static void builders()
{
	std::array<std::byte, 32> bytes{};
	bytes.fill(std::byte{0x55});
	auto built = client::makeList(bytes, 0x0807060504030201);
	CHECK(built.status == client::BuildStatus::Ok && built.written == wire::listRequestSize);
	CHECK(equals(Input(bytes).first(built.written),
	             std::array<unsigned char, 9>{1, 1, 2, 3, 4, 5, 6, 7, 8}));
	CHECK(bytes[9] == std::byte{0x55});
	built = client::makeStat(bytes, 0x12345678);
	CHECK(built.status == client::BuildStatus::Ok && built.written == wire::statRequestSize);
	CHECK(equals(Input(bytes).first(built.written),
	             std::array<unsigned char, 5>{2, 0x78, 0x56, 0x34, 0x12}));
	built = client::makeRead(bytes, 0x12345678, 0x0807060504030201);
	CHECK(built.status == client::BuildStatus::Ok && built.written == wire::readRequestSize);
	CHECK(equals(Input(bytes).first(built.written),
	             std::array<unsigned char, 13>{3, 0x78, 0x56, 0x34, 0x12, 1, 2, 3, 4, 5, 6, 7, 8}));
	const auto data = packet(std::array<unsigned char, 3>{0xa1, 0xb2, 0xc3});
	built = client::makeWrite(bytes, 0x12345678, 0x0807060504030201, data, true);
	CHECK(built.status == client::BuildStatus::Ok && built.written == 19);
	CHECK(equals(Input(bytes).first(built.written),
	             std::array<unsigned char, 19>{4, 0x78, 0x56, 0x34, 0x12, 1, 2, 3, 4, 5, 6, 7, 8, 1,
	                                           3, 0, 0xa1, 0xb2, 0xc3}));
	built = client::makeWrite(bytes, 0, 0, {}, false);
	CHECK(built.status == client::BuildStatus::Ok && built.written == 16 &&
	      bytes[13] == std::byte{} && bytes[14] == std::byte{} && bytes[15] == std::byte{});

	// Every short output is refused before any write, including empty storage.
	for (std::size_t capacity = 0; capacity != wire::readRequestSize; ++capacity) {
		bytes.fill(std::byte{0x55});
		built = client::makeRead(Output(bytes).first(capacity), 7, 9);
		CHECK(built.status == client::BuildStatus::BufferTooSmall && built.written == 0);
		CHECK(std::ranges::all_of(bytes, [](auto b) {
			return b == std::byte{0x55};
		}));
	}
	bytes.fill(std::byte{0x55});
	CHECK(client::makeList(Output(bytes).first(8), 0).status ==
	      client::BuildStatus::BufferTooSmall);
	CHECK(client::makeStat(Output(bytes).first(4), 0).status ==
	      client::BuildStatus::BufferTooSmall);
	CHECK(client::makeWrite(Output(bytes).first(18), 0, 0, data, true).status ==
	      client::BuildStatus::BufferTooSmall);
	CHECK(std::ranges::all_of(bytes, [](auto b) {
		return b == std::byte{0x55};
	}));
	std::array<std::byte, 13> fixed{};
	CHECK(client::makeRead(std::span<std::byte, 13>(fixed), 1, 2).written == fixed.size());

	std::vector<std::byte> large(wire::maxPayloadSize + 1, std::byte{0xab});
	std::vector<std::byte> output(wire::writeRequestHeaderSize + large.size(), std::byte{0x55});
	built = client::makeWrite(output, 0, 0, Input(large).first(wire::maxPayloadSize), true);
	CHECK(built.status == client::BuildStatus::Ok && built.written == 65551);
	CHECK(output[14] == std::byte{0xff} && output[15] == std::byte{0xff} &&
	      output[built.written - 1] == std::byte{0xab});
	output.assign(output.size(), std::byte{0x55});
	built = client::makeWrite(output, 0, 0, large, true);
	CHECK(built.status == client::BuildStatus::PayloadTooLarge && built.written == 0);
	CHECK(std::ranges::all_of(output, [](auto b) {
		return b == std::byte{0x55};
	}));

	// Input at the future header and at the future payload both remain valid.
	std::copy(data.begin(), data.end(), bytes.begin());
	built = client::makeWrite(bytes, 0, 0, Input(bytes).first(3), true);
	CHECK(built.written == 19 && std::equal(data.begin(), data.end(), bytes.begin() + 16));
	std::copy(data.begin(), data.end(), bytes.begin() + 16);
	CHECK(client::makeWrite(bytes, 0, 0, Input(bytes).subspan(16, 3), true).written == 19);
	CHECK(std::equal(data.begin(), data.end(), bytes.begin() + 16));
}

static void replies()
{
	const auto read =
	    packet(std::array<unsigned char, 15>{0, 1, 2, 3, 4, 5, 6, 7, 8, 1, 3, 0, 0xa1, 0xb2, 0xc3});
	const auto parsedRead = client::parseRead(read);
	CHECK(parsedRead && !parsedRead.shortError && parsedRead.response.status == Status::Ok);
	CHECK(parsedRead.response.next == 0x0807060504030201 && parsedRead.response.eof &&
	      parsedRead.response.data.size() == 3 &&
	      parsedRead.response.data.data() == read.data() + 12);
	truncatedAndTrailing(read, client::parseRead);
	auto badRead = read;
	badRead[0] = std::byte{9};
	CHECK(!client::parseRead(badRead));
	badRead = read;
	badRead[9] = std::byte{2};
	CHECK(!client::parseRead(badRead));
	badRead = read;
	badRead[10] = std::byte{4};
	CHECK(!client::parseRead(badRead));
	badRead = read;
	badRead[10] = std::byte{2};
	CHECK(!client::parseRead(badRead));
	badRead = read;
	badRead[0] = std::byte(Status::InvalidFile);
	CHECK(!client::parseRead(badRead));

	const auto stat = packet(std::array<unsigned char, 6>{0, 0x78, 0x56, 0x34, 0x12, 3});
	const auto parsedStat = client::parseStat(stat);
	CHECK(parsedStat && !parsedStat.shortError && parsedStat.response.status == Status::Ok &&
	      parsedStat.response.size == 0x12345678 &&
	      has(parsedStat.response.flags, FileFlag::Readable) &&
	      has(parsedStat.response.flags, FileFlag::Writable));
	truncatedAndTrailing(stat, client::parseStat);
	// Savable (4), and bits this client does not know, are kept: a flag a later
	// server adds never makes a STAT malformed.
	for (auto value : {4u, 0xffu}) {
		auto other = stat;
		other[5] = std::byte(value);
		const auto parsedOther = client::parseStat(other);
		CHECK(parsedOther && static_cast<unsigned>(parsedOther.response.flags) == value);
	}
	auto badStat = stat;
	badStat[0] = std::byte{9};
	CHECK(!client::parseStat(badStat));

	const auto write =
	    packet(std::array<unsigned char, 14>{0, 1, 2, 3, 4, 5, 6, 7, 8, 2, 0, 0, 0, 1});
	const auto parsedWrite = client::parseWrite(write, 3);
	CHECK(parsedWrite && !parsedWrite.shortError && parsedWrite.response.status == Status::Ok &&
	      parsedWrite.response.next == 0x0807060504030201 && parsedWrite.response.consumed == 2 &&
	      parsedWrite.response.complete);
	truncatedAndTrailing(write, [](Input data) {
		return client::parseWrite(data, 3);
	});
	CHECK(!client::parseWrite(write, 1));
	CHECK(!client::parseWrite(write, 65536));
	auto badWrite = write;
	badWrite[13] = std::byte{2};
	CHECK(!client::parseWrite(badWrite, 3));
	badWrite = write;
	badWrite[0] = std::byte{9};
	CHECK(!client::parseWrite(badWrite, 3));
	badWrite = write;
	badWrite[0] = std::byte(Status::NotWritable);
	CHECK(!client::parseWrite(badWrite, 3));
	badWrite[9] = std::byte{};
	CHECK(!client::parseWrite(badWrite, 3));
	badWrite[13] = std::byte{};
	CHECK(client::parseWrite(badWrite, 3).response.status == Status::NotWritable);
	badWrite[0] = std::byte{};
	CHECK(client::parseWrite(badWrite, 0)); // Opaque cursor-only progress is valid.

	for (unsigned code = 0; code <= 8; ++code) {
		std::array<std::byte, 12> error{};
		error[0] = std::byte(code);
		const auto result = client::parseRead(error);
		CHECK(result && result.response.status == static_cast<Status>(code));
	}
	for (unsigned code = 0; code <= 0xff; ++code) {
		const std::array<std::byte, 1> shortReply{std::byte(code)};
		const bool valid = code == static_cast<unsigned>(Status::InvalidData);
		const auto r = client::parseRead(shortReply);
		const auto l = client::parseList(shortReply);
		const auto s = client::parseStat(shortReply);
		const auto w = client::parseWrite(shortReply, 0);
		CHECK(bool(r) == valid && bool(l) == valid && bool(s) == valid && bool(w) == valid);
		if (valid) {
			CHECK(r.shortError && l.shortError && s.shortError && w.shortError);
			CHECK(r.response.status == Status::InvalidData &&
			      l.response.status == Status::InvalidData &&
			      s.response.status == Status::InvalidData &&
			      w.response.status == Status::InvalidData);
		}
	}
	std::vector<std::byte> maxRead(wire::chunkReplyHeaderSize + wire::maxPayloadSize);
	maxRead[9] = std::byte{1};
	maxRead[10] = maxRead[11] = std::byte{0xff};
	CHECK(client::parseRead(maxRead).response.data.size() == wire::maxPayloadSize);
	maxRead.push_back(std::byte{});
	CHECK(!client::parseRead(maxRead));
}

static void listing()
{
	const auto list = packet(std::array<unsigned char, 22>{
	    0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 10, 0, 2, 0, '/', 'a', 4, 0, '/', 'b', '/', 'c'});
	const auto parsed = client::parseList(list);
	CHECK(parsed && !parsed.shortError && parsed.response.status == Status::Ok &&
	      parsed.response.next == 2 && parsed.response.eof && parsed.response.paths.size() == 2);
	auto it = parsed.response.paths.begin();
	const auto first = *it++;
	CHECK(first == "/a" && first.data() == reinterpret_cast<const char*>(list.data() + 14));
	CHECK(*it++ == "/b/c" && it == parsed.response.paths.end());
	truncatedAndTrailing(list, client::parseList);
	auto bad = list;
	bad[12] = std::byte{11};
	CHECK(!client::parseList(bad));
	bad = list;
	bad[1] = std::byte{1};
	CHECK(!client::parseList(bad));
	bad = list;
	bad[9] = std::byte{2};
	CHECK(!client::parseList(bad));
	bad = list;
	bad[0] = std::byte{9};
	CHECK(!client::parseList(bad));
	bad = list;
	bad[0] = std::byte(Status::InvalidData);
	CHECK(!client::parseList(bad));
	const auto partial =
	    packet(std::array<unsigned char, 13>{0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2});
	CHECK(!client::parseList(partial)); // One dangling length-prefix byte.
	for (const auto path : std::array<std::string_view, 15>{
	         "", "/", "a", "ab", "//a", "/a/", "/a//b", "/.", "/..", "/a/./b", "/a/../b", "/a\\b",
	         "/a\0b"sv, "/a\nb", "/a\x7f"}) {
		std::vector<std::byte> bytes(14 + path.size());
		bytes[1] = std::byte{1};
		bytes[9] = std::byte{1};
		bytes[10] = std::byte(2 + path.size());
		bytes[12] = std::byte(path.size());
		if (!path.empty()) {
			std::memcpy(bytes.data() + 14, path.data(), path.size());
		}
		CHECK(!client::parseList(bytes));
	}
	std::vector<std::byte> maxPath(wire::chunkReplyHeaderSize + wire::maxPayloadSize,
	                               std::byte{'x'});
	std::fill_n(maxPath.begin(), 12, std::byte{});
	maxPath[1] = std::byte{1};
	maxPath[9] = std::byte{1};
	maxPath[10] = maxPath[11] = std::byte{0xff};
	maxPath[12] = std::byte{0xfd};
	maxPath[13] = std::byte{0xff};
	maxPath[14] = std::byte{'/'};
	const auto boundary = client::parseList(maxPath);
	CHECK(boundary && boundary.response.paths.size() == 1 &&
	      (*boundary.response.paths.begin()).size() == wire::maxListPathSize);
	maxPath.push_back(std::byte{'x'});
	maxPath[12] = std::byte{0xfe};
	CHECK(!client::parseList(maxPath));

	std::array<std::byte, 12> eof{};
	eof[9] = std::byte{1};
	CHECK(client::parseList(eof).response.paths.empty());
	eof[0] = std::byte(Status::InvalidCursor);
	CHECK(!client::parseList(eof));
	eof[9] = std::byte{};
	CHECK(client::parseList(eof).response.status == Status::InvalidCursor);
}

static void roundTrips()
{
	struct MutableBytes : BytesFile {
		Output data;

		explicit MutableBytes(std::array<std::byte, 3>& bytes) noexcept
		    : BytesFile(bytes), data(bytes)
		{}

		WriteResult write(Cursor cursor, Input input, bool final) noexcept
		{
			if (cursor > data.size()) {
				return {Status::InvalidCursor, cursor};
			}
			const auto count =
			    std::min(input.size(), data.size() - static_cast<std::size_t>(cursor));
			if (count != 0) {
				std::memmove(data.data() + cursor, input.data(), count);
			}
			return {Status::Ok, cursor + count, static_cast<std::uint32_t>(count),
			        final && count == input.size()};
		}
	};

	std::array<std::byte, 3> storage{std::byte{1}, std::byte{2}, std::byte{3}};
	MutableBytes provider(storage);
	const auto files = filesystem(file("/data", provider));
	std::array<std::byte, 32> request{};
	std::array<std::byte, 32> response{};
	const auto send = [&](client::BuildResult built) {
		CHECK(built.status == client::BuildStatus::Ok);
		const auto reply = resource::protocol::process(
		    files.view(), Input(request).first(built.written), response);
		CHECK(reply.written != 0);
		return Input(response).first(reply.written);
	};
	CHECK(client::parseList(send(client::makeList(request, 0))).response.paths.size() == 1);
	CHECK(client::parseStat(send(client::makeStat(request, 0))).response.size == storage.size());
	CHECK(client::parseRead(send(client::makeRead(request, 0, 0))).response.data.size() ==
	      storage.size());
	CHECK(client::parseRead(send(client::makeRead(request, 3, 4))).response.status ==
	      Status::InvalidFile);
	const auto writeData = packet(std::array<unsigned char, 2>{9, 8});
	const auto written = client::parseWrite(send(client::makeWrite(request, 0, 0, writeData, true)),
	                                        writeData.size());
	CHECK(written && written.response.consumed == 2 && written.response.complete &&
	      storage[0] == std::byte{9} && storage[1] == std::byte{8});
	request[0] = std::byte{0xff};
	const auto shortReply =
	    resource::protocol::process(files.view(), Input(request).first(1), response);
	const auto parsed = client::parseStat(Input(response).first(shortReply.written));
	CHECK(parsed && parsed.shortError && parsed.response.status == Status::InvalidData);
}

int main()
{
	builders();
	replies();
	listing();
	roundTrips();
	std::printf("Resource client: %u checks passed\n", checks);
}

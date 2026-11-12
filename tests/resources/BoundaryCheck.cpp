/**
 * @file BoundaryCheck.cpp
 * @brief Fixed-seed resource reply grammar and guarded request boundary checks.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The independent byte grammar does not use parser internals. Finite mutation
 * and random corpora exercise the public boundaries; they are not exhaustive.
 */
#include <resource/FileSystem.hpp>
#include <resource/protocol/Packets.hpp>
#include <resource/protocol/Protocol.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace client = resource::protocol::client;
namespace wire = resource::protocol::wire;
using namespace resource;

namespace {
unsigned checks = 0;
unsigned packets = 0;
std::array<unsigned, 5> accepted{};

#define CHECK(...)                                                                                 \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(__VA_ARGS__)) {                                                                      \
			std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__);                         \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

unsigned byte(Input input, std::size_t i)
{
	return std::to_integer<unsigned>(input[i]);
}

// Explicit LE arithmetic is independent of Client.hpp's templated load helper.
std::uint64_t word(Input input, std::size_t start, unsigned width)
{
	std::uint64_t result = 0;
	for (unsigned i = 0; i < width; ++i)
		result += std::uint64_t(byte(input, start + i)) << (i * 8);
	return result;
}

void put(Output output, std::size_t start, std::uint64_t value, unsigned width)
{
	for (unsigned i = 0; i < width; ++i)
		output[start + i] = std::byte((value >> (i * 8)) & 255);
}

bool shortReply(Input input)
{
	return input.size() == 1 && byte(input, 0) == static_cast<unsigned>(Status::InvalidData);
}

bool readGrammar(Input input)
{
	if (shortReply(input))
		return true;
	if (input.size() < 12 || byte(input, 0) > 8 || byte(input, 9) > 1)
		return false;
	const auto length = word(input, 10, 2);
	return input.size() - 12 == length &&
	       (byte(input, 0) == 0 || (length == 0 && byte(input, 9) == 0));
}

bool pathGrammar(Input input)
{
	if (input.size() < 2 || input.size() > 65533 || byte(input, 0) != '/' ||
	    byte(input, input.size() - 1) == '/')
		return false;
	std::size_t component = 1;
	for (std::size_t i = 1; i <= input.size(); ++i) {
		if (i != input.size() && byte(input, i) != '/') {
			if (byte(input, i) < 32 || byte(input, i) == 127 || byte(input, i) == '\\')
				return false;
			continue;
		}
		const auto length = i - component;
		if (length == 0 || (length == 1 && byte(input, component) == '.') ||
		    (length == 2 && byte(input, component) == '.' && byte(input, component + 1) == '.'))
			return false;
		component = i + 1;
	}
	return true;
}

bool listGrammar(Input input)
{
	if (!readGrammar(input))
		return false;
	if (shortReply(input))
		return true;
	std::size_t at = 12, count = 0;
	while (at < input.size()) {
		if (input.size() - at < 2)
			return false;
		const auto length = static_cast<std::size_t>(word(input, at, 2));
		at += 2;
		if (length > input.size() - at || !pathGrammar(input.subspan(at, length)))
			return false;
		at += length;
		++count;
	}
	return word(input, 1, 8) >= count;
}

bool statGrammar(Input input)
{
	return shortReply(input) || (input.size() == 6 && byte(input, 0) <= 8);
}

bool writeGrammar(Input input, std::size_t submitted)
{
	if (submitted > 65535)
		return false;
	if (shortReply(input))
		return true;
	if (input.size() != 14 || byte(input, 0) > 8 || byte(input, 13) > 1)
		return false;
	const auto consumed = word(input, 9, 4);
	return consumed <= submitted &&
	       (byte(input, 0) == 0 || (consumed == 0 && byte(input, 13) == 0));
}

bool saveGrammar(Input input)
{
	if (shortReply(input))
		return true;
	return input.size() == 10 && byte(input, 0) <= 8 && byte(input, 9) <= 1 &&
	       (byte(input, 0) == 0 || byte(input, 9) == 0);
}

// Accepted borrowed ranges must refer solely to this immutable packet.
bool inside(Input packet, const void* pointer, std::size_t size)
{
	const auto start = reinterpret_cast<std::uintptr_t>(packet.data());
	const auto address = reinterpret_cast<std::uintptr_t>(pointer);
	return address >= start && address - start <= packet.size() &&
	       size <= packet.size() - (address - start);
}

void inspect(Input input, std::size_t submitted)
{
	++packets;
	const auto read = client::parseRead(input);
	const auto list = client::parseList(input);
	const auto stat = client::parseStat(input);
	const auto write = client::parseWrite(input, submitted);
	const auto save = client::parseSave(input);
	CHECK(bool(read) == readGrammar(input));
	CHECK(bool(list) == listGrammar(input));
	CHECK(bool(stat) == statGrammar(input));
	CHECK(bool(write) == writeGrammar(input, submitted));
	CHECK(bool(save) == saveGrammar(input));
	const std::array<bool, 5> valid{bool(read), bool(list), bool(stat), bool(write), bool(save)};
	for (std::size_t i = 0; i < valid.size(); ++i)
		accepted[i] += valid[i];
	if (read) {
		CHECK(read.shortError == shortReply(input));
		if (!read.shortError) {
			CHECK(static_cast<unsigned>(read.response.status) == byte(input, 0));
			CHECK(read.response.next == word(input, 1, 8));
			CHECK(read.response.eof == (byte(input, 9) != 0));
			CHECK(read.response.data.size() == word(input, 10, 2));
			CHECK(inside(input, read.response.data.data(), read.response.data.size()));
		}
	}
	if (list) {
		CHECK(list.shortError == shortReply(input));
		std::size_t at = 12, count = 0;
		for (const auto path : list.response.paths) {
			CHECK(at + 2 <= input.size());
			const auto length = static_cast<std::size_t>(word(input, at, 2));
			CHECK(path.size() == length && inside(input, path.data(), path.size()));
			CHECK(path.data() == reinterpret_cast<const char*>(input.data() + at + 2));
			at += 2 + length;
			++count;
		}
		CHECK(count == list.response.paths.size());
		CHECK(list.shortError || at == input.size());
	}
	if (stat) {
		CHECK(stat.shortError == shortReply(input));
		if (!stat.shortError) {
			CHECK(stat.response.size == word(input, 1, 4));
			CHECK(static_cast<unsigned>(stat.response.flags) == byte(input, 5));
		}
	}
	if (write) {
		CHECK(write.shortError == shortReply(input));
		if (!write.shortError) {
			CHECK(write.response.next == word(input, 1, 8));
			CHECK(write.response.consumed == word(input, 9, 4));
			CHECK(write.response.complete == (byte(input, 13) != 0));
		}
	}
	if (save) {
		CHECK(save.shortError == shortReply(input));
		if (!save.shortError) {
			CHECK(static_cast<unsigned>(save.response.status) == byte(input, 0));
			CHECK(save.response.next == word(input, 1, 8));
			CHECK(save.response.complete == (byte(input, 9) != 0));
		}
	}
}

std::uint32_t next(std::uint32_t& seed)
{
	seed = seed * 1664525u + 1013904223u;
	return seed;
}

void replyCorpus()
{
	// Success/error seeds, a binary READ, two LIST records, both WRITE states and
	// both SAVE states.
	const std::array<std::size_t, 10> lengths{12, 12, 15, 22, 6, 14, 14, 1, 10, 10};
	std::array<std::array<std::byte, 64>, 10> seeds{};
	seeds[1][0] = std::byte(Status::InvalidCursor);
	seeds[2][9] = std::byte{1};
	put(seeds[2], 10, 3, 2);
	seeds[2][12] = std::byte{0xff};
	put(seeds[3], 1, 2, 8);
	seeds[3][9] = std::byte{1};
	put(seeds[3], 10, 10, 2);
	put(seeds[3], 12, 2, 2);
	seeds[3][14] = std::byte{'/'};
	seeds[3][15] = std::byte{'a'};
	put(seeds[3], 16, 4, 2);
	seeds[3][18] = std::byte{'/'};
	seeds[3][19] = std::byte{'b'};
	seeds[3][20] = std::byte{'/'};
	seeds[3][21] = std::byte{'c'};
	put(seeds[4], 1, 0xfedcba98, 4);
	seeds[4][5] = std::byte{3};
	put(seeds[5], 9, 2, 4);
	seeds[5][13] = std::byte{1};
	seeds[6][0] = std::byte(Status::NotWritable);
	seeds[7][0] = std::byte(Status::InvalidData);
	put(seeds[8], 1, 4, 8);
	seeds[8][9] = std::byte{1};
	seeds[9][0] = std::byte(Status::NotWritable);
	put(seeds[9], 1, 2, 8);
	for (std::size_t row = 0; row < seeds.size(); ++row) {
		const auto length = lengths[row];
		for (std::size_t size = 0; size <= length + 1; ++size)
			inspect(Input(seeds[row]).first(size), 3);
		for (std::size_t offset = 0; offset < length; ++offset) {
			for (unsigned value = 0; value <= 255; ++value) {
				auto changed = seeds[row];
				changed[offset] = std::byte(value);
				inspect(Input(changed).first(length), 3);
			}
		}
	}
	std::uint32_t seed = 0x3bd139a5;
	std::array<std::byte, 64> random{};
	for (unsigned trial = 0; trial < 10000; ++trial) {
		for (auto& item : random)
			item = std::byte(next(seed) >> 24);
		const auto size = static_cast<std::size_t>(next(seed) % 65);
		// Biased valid envelopes prevent a corpus of only trivial rejections.
		if (trial % 4 == 0 && size >= 12) {
			random[0] = std::byte{};
			random[9] = std::byte(trial % 2);
			put(random, 10, size - 12, 2);
		}
		inspect(Input(random).first(size), trial % 3 == 0 ? 65536 : next(seed) % 65536);
	}
	for (const auto count : accepted)
		CHECK(count > 100);
}

// A conforming provider counts all callbacks and writes only the supplied span.
// Public methods: size() inspect length; read() copy bytes; write() consume bytes;
// save() persist in three steps.
struct Provider {
	std::array<std::byte, 8> data{};
	mutable unsigned sizes = 0, reads = 0;
	unsigned writes = 0, saves = 0;

	FileSize size() const noexcept
	{
		++sizes;
		return data.size();
	}

	ReadResult read(Cursor cursor, Output output) const noexcept
	{
		++reads;
		if (cursor > data.size())
			return {Status::InvalidCursor, cursor};
		if (output.empty() && cursor != data.size())
			return {Status::BufferTooSmall, cursor};
		const auto count = std::min(output.size(), data.size() - static_cast<std::size_t>(cursor));
		if (count != 0)
			std::memcpy(output.data(), data.data() + cursor, count);
		return {Status::Ok, cursor + count, static_cast<std::uint32_t>(count),
		        cursor + count == data.size()};
	}

	WriteResult write(Cursor cursor, Input input, bool final) noexcept
	{
		++writes;
		if (cursor > data.size())
			return {Status::InvalidCursor, cursor};
		if (!input.empty() && cursor == data.size())
			return {Status::BufferTooSmall, cursor};
		const auto count = std::min(input.size(), data.size() - static_cast<std::size_t>(cursor));
		if (count != 0)
			std::memcpy(data.data() + cursor, input.data(), count);
		return {Status::Ok, cursor + count, static_cast<std::uint32_t>(count),
		        final && count == input.size()};
	}

	SaveResult save(Cursor cursor) noexcept
	{
		++saves;
		if (cursor > 2)
			return {Status::InvalidCursor, cursor};
		return cursor < 2 ? SaveResult{Status::Ok, cursor + 1, false}
		                  : SaveResult{Status::Ok, cursor, true};
	}
};

bool requestGrammar(Input input)
{
	if (input.empty())
		return false;
	switch (byte(input, 0)) {
		case 1:
			return input.size() == 9;
		case 2:
			return input.size() == 5;
		case 3:
			return input.size() == 13;
		case 4:
			return input.size() >= 16 && byte(input, 13) <= 1 &&
			       word(input, 14, 2) == input.size() - 16;
		case 5:
			return input.size() == 13;
		default:
			return false;
	}
}

void requestCorpus()
{
	Provider provider;
	const auto files = filesystem(file("/bytes", provider));
	std::array<std::byte, 64> request{};
	std::array<std::byte, 66> response{};
	std::uint32_t seed = 0x81294abc;
	for (unsigned trial = 0; trial < 5000; ++trial) {
		for (auto& item : request)
			item = std::byte(next(seed) >> 24);
		std::size_t size = next(seed) % 65;
		if (trial % 2 == 0) {
			request[0] = std::byte(1 + (trial / 2) % 5);
			const auto op = byte(request, 0);
			size = op == 1 ? 9 : op == 2 ? 5 : op == 3 || op == 5 ? 13 : 16 + trial % 7;
			put(request, 1, 0, 4);
			if (op == 1)
				put(request, 1, trial % 3, 8);
			if (op >= 3)
				put(request, 5, op == 5 ? trial % 4 : trial % 10, 8);
			if (op == 4) {
				request[13] = std::byte(trial % 3);
				put(request, 14, size - 16, 2);
			}
		}
		const auto input = Input(request).first(size);
		const auto original = request;
		const auto callbacks = provider.sizes + provider.reads + provider.writes + provider.saves;
		const auto capacity = static_cast<std::size_t>(trial % 65);
		response.fill(std::byte{0xa5});
		const auto result =
		    resource::protocol::process(files.view(), input, Output(response).subspan(1, capacity));
		CHECK(result.written <= capacity && request == original);
		CHECK(response.front() == std::byte{0xa5});
		CHECK(std::all_of(response.begin() + 1 + capacity, response.end(), [](auto value) {
			return value == std::byte{0xa5};
		}));
		if (!requestGrammar(input)) {
			CHECK(result.status == Status::InvalidData);
			CHECK(provider.sizes + provider.reads + provider.writes + provider.saves == callbacks);
			CHECK(result.written == (capacity == 0 ? 0 : 1));
		}
		if (result.written != 0) {
			const auto reply = Input(response).subspan(1, result.written);
			if (!requestGrammar(input))
				CHECK(shortReply(reply));
			else {
				switch (byte(input, 0)) {
					case 1:
						CHECK(client::parseList(reply));
						break;
					case 2:
						CHECK(client::parseStat(reply));
						break;
					case 3:
						CHECK(client::parseRead(reply));
						break;
					case 4:
						CHECK(client::parseWrite(reply, size - 16));
						break;
					case 5:
						CHECK(client::parseSave(reply));
						break;
				}
			}
		}
	}
	CHECK(provider.sizes > 100);
	CHECK(provider.reads > 100);
	CHECK(provider.writes > 100);
	CHECK(provider.saves > 100);
}
} // namespace

int main()
{
	replyCorpus();
	requestCorpus();
	std::printf("Resource boundaries: %u checks passed\n", checks);
	std::printf("Reply corpus: %u packets; accepted read/list/stat/write/save %u/%u/%u/%u/%u\n",
	            packets, accepted[0], accepted[1], accepted[2], accepted[3], accepted[4]);
}

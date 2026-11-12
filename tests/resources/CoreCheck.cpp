// Resource core and packet contracts. Authors: Ruslan Kovtun, codexAi (MIT).
// Checks provider capabilities, resource lookup and bounded packet-level file operations.
// Canaries and deliberately inconsistent provider reports verify count, cursor and progress validation.

#include <resource/FileSystem.hpp>
#include <resource/ChunkWriter.hpp>
#include <resource/protocol/Packets.hpp>
#include <resource/protocol/Protocol.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

using namespace resource;
int checks = 0;
#define CHECK(...)                                                                                 \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(__VA_ARGS__)) {                                                                      \
			std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__);                         \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

// Mutable byte provider with operation counters and checked partial transfers.
// Public methods:
// - size(): Count size requests.
// - read(): Serve bounded bytes.
// - write(): Apply bounded bytes.
struct Memory {
	std::array<std::byte, 10> bytes{};
	mutable unsigned reads = 0;
	mutable unsigned stats = 0;
	unsigned writes = 0;

	FileSize size() const noexcept
	{
		++stats;
		return static_cast<FileSize>(bytes.size());
	}

	ReadResult read(Cursor cursor, Output out) const noexcept
	{
		++reads;
		if (cursor > bytes.size()) {
			return {Status::InvalidCursor, cursor};
		}
		auto count = std::min(out.size(), bytes.size() - static_cast<std::size_t>(cursor));
		if (count == 0 && cursor != bytes.size()) {
			return {Status::BufferTooSmall, cursor};
		}
		if (count) {
			std::memcpy(out.data(), bytes.data() + cursor, count);
		}
		return {Status::Ok, cursor + count, static_cast<std::uint32_t>(count),
		        cursor + count == bytes.size()};
	}

	WriteResult write(Cursor cursor, Input in, bool final) noexcept
	{
		++writes;
		if (cursor > bytes.size()) {
			return {Status::InvalidCursor, cursor};
		}
		auto count = std::min(in.size(), bytes.size() - static_cast<std::size_t>(cursor));
		if (count) {
			std::memmove(bytes.data() + cursor, in.data(), count);
		}
		return {Status::Ok, cursor + count, static_cast<std::uint32_t>(count),
		        final && count == in.size()};
	}
};

// Empty readable provider used to check read-only capability selection.
// Public methods:
// - size(): Report empty extent.
// - read(): Report empty stream.
struct ReadOnly {
	FileSize size() const noexcept
	{
		return 0;
	}

	ReadResult read(Cursor c, Output) const noexcept
	{
		return c ? ReadResult{Status::InvalidCursor, c} : ReadResult{Status::Ok, 0, 0, true};
	}
};

// Write-only provider advances an opaque cursor independently of byte count.
// Public methods:
// - size(): Report empty extent.
// - write(): Advance opaque cursor.
struct WriteOnly {
	FileSize size() const noexcept
	{
		return 0;
	}

	WriteResult write(Cursor c, Input in, bool final) noexcept
	{
		return {Status::Ok, c + 7, static_cast<std::uint32_t>(in.size()), final};
	}
};

// Throwing read signature must fail the provider noexcept contract.
// Public methods:
// - size(): Report empty extent.
// - read(): Expose throwing callback.
struct Throwing {
	FileSize size() const noexcept
	{
		return 0;
	}

	ReadResult read(Cursor, Output)
	{
		return {};
	}
};

// Wide size return type must fail the exact FileSize contract.
// Public methods:
// - size(): Expose wide size.
// - read(): Provide read signature.
struct WrongSize {
	std::uint64_t size() const noexcept
	{
		return 0;
	}

	ReadResult read(Cursor, Output) const noexcept
	{
		return {};
	}
};

// Provider intentionally reports transfers beyond the supplied buffer extent.
// Public methods:
// - size(): Report empty extent.
// - read(): Overreport written bytes.
// - write(): Overreport consumed bytes.
struct Misreport {
	FileSize size() const noexcept
	{
		return 0;
	}

	ReadResult read(Cursor c, Output out) const noexcept
	{
		return {Status::Ok, c, static_cast<std::uint32_t>(out.size() + 1), true};
	}

	WriteResult write(Cursor c, Input in, bool) noexcept
	{
		return {Status::Ok, c, static_cast<std::uint32_t>(in.size() + 1), false};
	}
};

static_assert(!Provider<Throwing> && !Provider<WrongSize>);
static_assert(std::is_standard_layout_v<ReadResult> && std::is_trivially_copyable_v<ReadResult>);
static_assert(std::is_standard_layout_v<WriteResult> && std::is_trivially_copyable_v<WriteResult>);
static_assert(std::is_standard_layout_v<FileStat> && std::is_trivially_copyable_v<FileStat>);
constexpr ReadResult defaultRead;
constexpr WriteResult defaultWrite;
constexpr FileStat defaultStat;
static_assert(defaultRead.status == Status::Ok && defaultRead.next == 0 &&
              defaultRead.written == 0 && !defaultRead.eof);
static_assert(defaultWrite.status == Status::Ok && defaultWrite.next == 0 &&
              defaultWrite.consumed == 0 && !defaultWrite.complete);
static_assert(defaultStat.status == Status::Ok && defaultStat.size == 0 &&
              defaultStat.flags == FileFlag::None);
constexpr ReadResult fullRead{Status::CursorExpired, 0x100000007ull, 23, true};
constexpr WriteResult fullWrite{Status::InvalidCursor, 0x100000009ull, 29, true};
constexpr FileStat fullStat{Status::InvalidData, 31, FileFlag::Writable};
static_assert(fullRead.status == Status::CursorExpired && fullRead.next == 0x100000007ull &&
              fullRead.written == 23 && fullRead.eof);
static_assert(fullWrite.status == Status::InvalidCursor && fullWrite.next == 0x100000009ull &&
              fullWrite.consumed == 29 && fullWrite.complete);
static_assert(fullStat.status == Status::InvalidData && fullStat.size == 31 &&
              fullStat.flags == FileFlag::Writable);
static_assert(Provider<Memory> && Provider<const Memory> && !WritableProvider<const Memory>);
Memory memory;
const ReadOnly ro;
WriteOnly wo;
inline constexpr auto files =
    filesystem(file("/data.bin", memory), file("/ro", ro), file("/wo", wo));
static_assert(files.fileCount() == 3 && files.path(0) == "/data.bin");
static_assert(std::is_standard_layout_v<FileEntry> &&
              std::is_trivially_copy_constructible_v<FileEntry> &&
              std::is_trivially_destructible_v<FileEntry>);
constinit auto constantFs = filesystem(file("/value", memory));

template<class T>
void put(Output out, std::size_t p, T v)
{
	for (std::size_t i = 0; i < sizeof(T); ++i) {
		out[p + i] = std::byte((std::uint64_t(v) >> (8 * i)) & 255);
	}
}

std::uint64_t get(Input in, std::size_t p, std::size_t n)
{
	std::uint64_t r = 0;
	for (std::size_t i = 0; i < n; ++i) {
		r |= std::uint64_t(std::to_integer<unsigned>(in[p + i])) << (8 * i);
	}
	return r;
}

void progressChecks()
{
	// Exercise each independent form of progress, including a cursor change
	// confined to the high word. Final input does not imply provider completion.
	// Configurable provider separates opaque cursor movement from transfer and completion reports.
	// Public methods:
	// - size(): Report fixed extent.
	// - read(): Simulate read progress.
	// - write(): Simulate write progress.
	struct Progress {
		bool advance = false;
		bool finish = false;
		std::uint32_t count = 0;
		Status status = Status::Ok;
		mutable unsigned calls = 0;

		FileSize size() const noexcept
		{
			return 1;
		}

		ReadResult read(Cursor cursor, Output out) const noexcept
		{
			++calls;
			if (count != 0 && !out.empty()) {
				out[0] = std::byte{0x5a};
			}
			return {status, advance ? cursor + (Cursor{1} << 32) : cursor, count, finish};
		}

		WriteResult write(Cursor cursor, Input, bool) noexcept
		{
			++calls;
			return {status, advance ? cursor + (Cursor{1} << 32) : cursor, count, finish};
		}
	} provider;

	const auto fs = filesystem(file("/progress", provider));
	constexpr Cursor cursor = 0x0102030405060708ull;
	std::array<std::byte, 17> request{};
	std::array<std::byte, 32> response{};
	put(Output{request}, 5, cursor);
	put(Output{request}, 14, std::uint16_t{1});
	request[16] = std::byte{0x5a};
	using resource::protocol::process;
	for (unsigned mode = 0; mode < 8; ++mode) {
		provider.advance = (mode & 1) != 0;
		provider.finish = (mode & 2) != 0;
		provider.count = (mode & 4) ? 1 : 0;
		const bool stalled = mode == 0;
		const auto expectedStatus = stalled ? Status::InternalError : Status::Ok;
		const auto expectedCursor = provider.advance ? cursor + (Cursor{1} << 32) : cursor;
		request[0] = std::byte{3};
		const auto read = process(fs.view(), Input{request}.first(13), response);
		CHECK(read.status == expectedStatus && read.written == 12 + provider.count);
		CHECK(response[0] == std::byte(expectedStatus) && get(response, 1, 8) == expectedCursor);
		CHECK(get(response, 10, 2) == provider.count && response[9] == std::byte(provider.finish));
		if (provider.count) {
			CHECK(response[12] == std::byte{0x5a});
		}
		request[0] = std::byte{4};
		for (unsigned final = 0; final <= 1; ++final) {
			request[13] = std::byte(final);
			const auto written = process(fs.view(), request, response);
			CHECK(written.status == expectedStatus && written.written == 14);
			CHECK(response[0] == std::byte(expectedStatus) &&
			      get(response, 1, 8) == expectedCursor);
			CHECK(get(response, 9, 4) == provider.count &&
			      response[13] == std::byte(provider.finish));
		}
	}
	CHECK(provider.calls == 24);

	// A provider can legitimately ask for a larger buffer with no progress;
	// the guard must preserve this error rather than inventing InternalError.
	provider.advance = provider.finish = false;
	provider.count = 0;
	provider.status = Status::BufferTooSmall;
	for (unsigned op : {3u, 4u}) {
		request[0] = std::byte(op);
		const auto result = process(fs.view(), Input{request}.first(op == 3 ? 13 : 17), response);
		CHECK(result.status == Status::BufferTooSmall && get(response, 1, 8) == cursor);
		CHECK(get(response, op == 3 ? 10 : 9, op == 3 ? 2 : 4) == 0);
	}
}

// SAVE persists what a file holds, a step per request until complete; a file
// without save() answers NotWritable; every answer keeps the envelope;
// malformed lengths and unknown operations are the one-byte InvalidData.
void saveChecks()
{
	using resource::protocol::process;
	namespace rc = resource::protocol::client;

	// Keeps its bytes in steps: two moves, then complete; or refuses, or stalls,
	// or answers an error that moves its cursor or says complete.
	struct Steps {
		unsigned steps = 0;
		Status status = Status::Ok;
		bool stall = false, errorMoves = false, errorCompletes = false;

		FileSize size() const noexcept
		{
			return 1;
		}

		ReadResult read(Cursor cursor, Output) const noexcept
		{
			return {Status::Ok, cursor, 0, true};
		}

		SaveResult save(Cursor cursor) noexcept
		{
			if (status != Status::Ok)
				return {status, errorMoves ? cursor + 1 : cursor, errorCompletes};
			if (stall)
				return {Status::Ok, cursor, false};
			++steps;
			return cursor < 2 ? SaveResult{Status::Ok, cursor + 1, false}
			                  : SaveResult{Status::Ok, cursor, true};
		}
	} steps;

	// Keeps them at once, or refuses.
	struct Once {
		unsigned saves = 0;
		Status status = Status::Ok;

		FileSize size() const noexcept
		{
			return 1;
		}

		ReadResult read(Cursor cursor, Output) const noexcept
		{
			return {Status::Ok, cursor, 0, true};
		}

		SaveResult save(Cursor cursor) noexcept
		{
			++saves;
			return status == Status::Ok ? SaveResult{Status::Ok, cursor, true}
			                            : SaveResult{status, cursor};
		}
	} once;

	// Has nothing to persist.
	struct Plain {
		FileSize size() const noexcept
		{
			return 1;
		}

		ReadResult read(Cursor cursor, Output) const noexcept
		{
			return {Status::Ok, cursor, 0, true};
		}
	} plain;

	static_assert(SavableProvider<decltype(steps)> && !SavableProvider<decltype(plain)>);
	const auto fs = filesystem(file("/steps", steps), file("/once", once), file("/plain", plain));
	CHECK(fs.stat(0).flags == (FileFlag::Readable | FileFlag::Savable) &&
	      fs.stat(1).flags == (FileFlag::Readable | FileFlag::Savable) &&
	      fs.stat(2).flags == FileFlag::Readable);
	std::array<std::byte, 13> request{};
	std::array<std::byte, 16> response{};
	const auto save = [&](FileIndex index, Cursor cursor) {
		request.fill(std::byte{0});
		request[0] = std::byte{5};
		put(Output{request}, 1, index);
		put(Output{request}, 5, cursor);
		return process(fs.view(), request, response);
	};
	// In steps, from cursor 0 until complete.
	Cursor cursor = 0;
	unsigned replies = 0;
	for (bool done = false; !done && replies < 10; ++replies) {
		const auto step = save(0, cursor);
		CHECK(step.status == Status::Ok && step.written == 10);
		cursor = get(response, 1, 8);
		done = response[9] == std::byte{1};
	}
	CHECK(replies == 3 && steps.steps == 3 && cursor == 2);
	// At once: one complete answer, the cursor echoed; its refusal is the answer.
	auto result = save(1, 3);
	CHECK(result.status == Status::Ok && response[9] == std::byte{1} && get(response, 1, 8) == 3 &&
	      once.saves == 1);
	once.status = Status::InternalError;
	result = save(1, 0);
	CHECK(result.status == Status::InternalError &&
	      response[0] == std::byte(Status::InternalError) && response[9] == std::byte{0} &&
	      get(response, 1, 8) == 0 && once.saves == 2);
	once.status = Status::Ok;
	// Nothing to persist, or no such file: in the envelope, the cursor kept.
	result = save(2, 7);
	CHECK(result.status == Status::NotWritable && result.written == 10 && get(response, 1, 8) == 7);
	result = save(9, 0);
	CHECK(result.status == Status::InvalidFile && result.written == 10);
	// A step that neither completes nor moves: InternalError; an error the
	// provider gives keeps the cursor.
	steps.stall = true;
	result = save(0, 5);
	CHECK(result.status == Status::InternalError && get(response, 1, 8) == 5 &&
	      response[9] == std::byte{0});
	steps.stall = false;
	steps.status = Status::CursorExpired;
	result = save(0, 4);
	CHECK(result.status == Status::CursorExpired && get(response, 1, 8) == 4);
	// An error that moves the cursor or says complete is not one: InternalError,
	// the cursor kept, not complete.
	steps.errorMoves = true;
	result = save(0, 4);
	CHECK(result.status == Status::InternalError && get(response, 1, 8) == 4 &&
	      response[9] == std::byte{0});
	steps.errorMoves = false;
	steps.errorCompletes = true;
	result = save(0, 4);
	CHECK(result.status == Status::InternalError && get(response, 1, 8) == 4 &&
	      response[9] == std::byte{0});
	steps.errorCompletes = false;
	steps.status = Status::Ok;
	// Malformed lengths: the one-byte InvalidData; a small reply buffer: BufferTooSmall.
	request.fill(std::byte{0});
	request[0] = std::byte{5};
	for (std::size_t n = 1; n < 13; ++n) {
		const auto shortRequest = process(fs.view(), Input{request}.first(n), response);
		CHECK(shortRequest.status == Status::InvalidData && shortRequest.written == 1);
	}
	std::array<std::byte, 14> longer{};
	longer[0] = std::byte{5};
	CHECK(process(fs.view(), longer, response).written == 1);
	CHECK(process(fs.view(), request, Output{response}.first(9)).status == Status::BufferTooSmall);
	// Operation 6 is still unknown.
	request[0] = std::byte{6};
	const auto unknown = process(fs.view(), request, response);
	CHECK(unknown.status == Status::InvalidData && unknown.written == 1);
	// The client's packets: built, answered, parsed.
	std::array<std::byte, 13> built{};
	CHECK(rc::makeSave(built, 1, 0).written == 13 && built[0] == std::byte{5});
	CHECK(rc::makeSave(Output{built}.first(12), 1, 0).status == rc::BuildStatus::BufferTooSmall);
	const auto answered = process(fs.view(), built, response);
	const auto parsed = rc::parseSave(Input{response}.first(answered.written));
	CHECK(parsed && parsed.response.status == Status::Ok && parsed.response.complete &&
	      once.saves == 3);
	CHECK(!rc::parseSave(Input{response}.first(9)));
	std::array<std::byte, 10> claims{};
	claims[0] = std::byte(Status::InvalidData);
	claims[9] = std::byte{1};
	CHECK(!rc::parseSave(claims));
	const std::array<std::byte, 1> shortError{std::byte(Status::InvalidData)};
	CHECK(rc::parseSave(shortError) && rc::parseSave(shortError).shortError);
}

int main()
{
	saveChecks();
	progressChecks();
	std::array<std::byte, 128> out{};
	CHECK(files.path(3).empty());
	CHECK(files.stat(3).status == Status::InvalidFile);
	CHECK(has(files.stat(0).flags, FileFlag::Readable | FileFlag::Writable));
	CHECK(files.stat(1).flags == FileFlag::Readable && files.stat(2).flags == FileFlag::Writable);
	CHECK(files.read(2, 123, out).status == Status::NotReadable);
	CHECK(files.write(1, 123, {}).status == Status::NotWritable);
	CHECK(files.read(std::numeric_limits<FileIndex>::max(), 9, out).next == 9);
	CHECK(files.write(9, 7, {}).next == 7);
	CHECK(files.read(0, std::numeric_limits<Cursor>::max(), out).status == Status::InvalidCursor);
	CHECK(filesystem().fileCount() == 0);
	CHECK(FileSystemView{}.stat(0).status == Status::InvalidFile);
	const auto view = files.view();
	CHECK(view.fileCount() == 3 && constantFs.fileCount() == 1);
	ChunkWriter writer{Output{out}.first(3)};
	CHECK(!writer.writeAtomic(Input{out}.first(4)) && writer.empty());
	CHECK(writer.writePartial(Input{out}.first(4)) == 3 && writer.remaining() == 0);
	CHECK(writer.writeAtomic({}));
	ChunkWriter empty{{}};
	CHECK(empty.writePartial({}) == 0 && empty.writeAtomic({}));

	std::array<std::byte, 64> request{};
	using resource::protocol::process;
	request[0] = std::byte{1};
	const auto statCalls = memory.stats;
	auto r = process(view, Input{request}.first(9), Output{out}.first(23));
	CHECK(r.status == Status::Ok && r.written == 23 && get(out, 1, 8) == 1 &&
	      out[9] == std::byte{0});
	CHECK(get(out, 12, 2) == 9 && std::memcmp(out.data() + 14, "/data.bin", 9) == 0);
	CHECK(memory.stats == statCalls);
	put(Output{request}, 1, Cursor{1});
	r = process(view, Input{request}.first(9), out);
	CHECK(r.status == Status::Ok && get(out, 1, 8) == 3 && out[9] == std::byte{1});
	put(Output{request}, 1, Cursor{3});
	CHECK(process(view, Input{request}.first(9), out).written == 12 && out[9] == std::byte{1});
	put(Output{request}, 1, Cursor{4});
	CHECK(process(view, Input{request}.first(9), out).status == Status::InvalidCursor &&
	      get(out, 1, 8) == 4);
	put(Output{request}, 1, Cursor{0});
	CHECK(process(view, Input{request}.first(9), Output{out}.first(22)).status ==
	          Status::BufferTooSmall &&
	      get(out, 1, 8) == 0);
	// Generic resource paths have no protocol length limit. LIST must separate
	// a small caller buffer from a path no possible v1 packet can represent.
	for (std::size_t length : {65533u, 65534u, 65535u, 65536u}) {
		std::string label(length, 'x');
		label.front() = '/';
		const auto longFs = filesystem(file(std::string_view{label}, memory));
		CHECK(longFs.path(0).size() == length);
		std::vector<std::byte> guarded(12 + 65535 + 2, std::byte{0xa5});
		auto reply = Output{guarded}.subspan(1, 12 + 65535);
		const auto result = process(longFs.view(), Input{request}.first(9), reply);
		if (length == 65533) {
			CHECK(result.status == Status::Ok && result.written == reply.size() &&
			      get(reply, 1, 8) == 1 && reply[9] == std::byte{1} && get(reply, 10, 2) == 65535 &&
			      get(reply, 12, 2) == length &&
			      std::memcmp(reply.data() + 14, label.data(), length) == 0);
			CHECK(process(longFs.view(), Input{request}.first(9), reply.first(reply.size() - 1))
			          .status == Status::BufferTooSmall);
		} else {
			CHECK(result.status == Status::InvalidData && result.written == 12 &&
			      get(reply, 1, 8) == 0 && get(reply, 10, 2) == 0 && reply[9] == std::byte{0});
			CHECK(process(longFs.view(), Input{request}.first(9), reply.first(12)).status ==
			      Status::InvalidData);
		}
		CHECK(guarded.front() == std::byte{0xa5} && guarded.back() == std::byte{0xa5});
		CHECK(memory.stats == statCalls);
	}
	// A later unrepresentable path must not hide the preceding valid page.
	std::string oversized(65534, 'x');
	oversized.front() = '/';
	const auto partialFs =
	    filesystem(file("/a", memory), file(std::string_view{oversized}, memory));
	for (std::size_t capacity : {4u, 5u, 100u, 65535u}) {
		std::vector<std::byte> page(12 + capacity, std::byte{0xa5});
		put(Output{request}, 1, Cursor{0});
		const auto first = process(partialFs.view(), Input{request}.first(9), page);
		CHECK(first.status == Status::Ok && first.written == 16 && get(page, 1, 8) == 1 &&
		      get(page, 10, 2) == 4 && page[9] == std::byte{0});
		CHECK(get(page, 12, 2) == 2 && page[14] == std::byte{'/'} && page[15] == std::byte{'a'});
		put(Output{request}, 1, Cursor{1});
		const auto second = process(partialFs.view(), Input{request}.first(9), page);
		CHECK(second.status == Status::InvalidData && second.written == 12 &&
		      get(page, 1, 8) == 1 && get(page, 10, 2) == 0);
	}
	put(Output{request}, 1, Cursor{0});
	request[0] = std::byte{2};
	CHECK(process(view, Input{request}.first(5), out).written == 6 && get(out, 1, 4) == 10 &&
	      out[5] == std::byte{3});
	put(Output{request}, 1, std::uint32_t{0xffffffffu});
	CHECK(process(view, Input{request}.first(5), out).status == Status::InvalidFile);
	put(Output{request}, 1, std::uint32_t{0});
	request[0] = std::byte{4};
	put(Output{request}, 5, Cursor{0});
	request[13] = std::byte{1};
	put(Output{request}, 14, std::uint16_t{3});
	request[16] = std::byte{10};
	request[17] = std::byte{20};
	request[18] = std::byte{30};
	auto writes = memory.writes;
	CHECK(process(view, Input{request}.first(19), Output{out}.first(13)).status ==
	          Status::BufferTooSmall &&
	      memory.writes == writes);
	r = process(view, Input{request}.first(19), out);
	CHECK(r.status == Status::Ok && r.written == 14 && get(out, 1, 8) == 3 && get(out, 9, 4) == 3 &&
	      out[13] == std::byte{1});
	(void)process(view, Input{request}.first(19), out);
	CHECK(memory.writes == writes + 2 && memory.bytes[1] == std::byte{20});
	request[13] = std::byte{2};
	CHECK(process(view, Input{request}.first(19), out).status == Status::InvalidData &&
	      memory.writes == writes + 2);
	request[13] = std::byte{1};
	CHECK(process(view, Input{request}.first(18), out).status == Status::InvalidData);
	CHECK(process(view, Input{request}.first(20), out).status == Status::InvalidData);
	request[0] = std::byte{3};
	r = process(view, Input{request}.first(13), out);
	CHECK(r.status == Status::Ok && get(out, 10, 2) == 10 && out[12] == std::byte{10});
	CHECK(process(view, Input{request}.first(13), Output{out}.first(12)).status ==
	      Status::BufferTooSmall);
	// Equal begin/end cursors on an empty file are valid EOF, even with no
	// READ payload capacity. Provider count mistakes cannot corrupt envelopes.
	put(Output{request}, 1, std::uint32_t{1});
	CHECK(process(view, Input{request}.first(13), Output{out}.first(12)).status == Status::Ok &&
	      out[9] == std::byte{1});
	Misreport wrong;
	const auto wrongFs = filesystem(file("/wrong", wrong));
	put(Output{request}, 1, std::uint32_t{0});
	CHECK(process(wrongFs.view(), Input{request}.first(13), out).status == Status::InternalError);
	request[0] = std::byte{4};
	CHECK(process(wrongFs.view(), Input{request}.first(19), out).status == Status::InternalError);
	// In-place WRITE parses its input before the response overwrites it.
	request[0] = std::byte{4};
	CHECK(process(view, Input{request}.first(19), request).status == Status::Ok &&
	      memory.bytes[0] == std::byte{10});
	// All request fields are captured before an in-place reply overwrites them.
	for (unsigned op : {1u, 2u, 3u}) {
		request.fill(std::byte{0});
		request[0] = std::byte(op);
		const std::size_t length = op == 1 ? 9 : op == 2 ? 5 : 13;
		auto shared = request;
		const auto separate = process(view, Input{request}.first(length), out);
		const auto inplace = process(view, Input{shared}.first(length), shared);
		CHECK(inplace.status == separate.status && inplace.written == separate.written &&
		      std::equal(out.begin(), out.begin() + separate.written, shared.begin()));
	}
	// Empty final WRITE is delivered once; it can finish a provider transaction.
	request.fill(std::byte{0});
	request[0] = std::byte{4};
	request[13] = std::byte{1};
	writes = memory.writes;
	const auto emptyWrite = process(view, Input{request}.first(16), out);
	CHECK(emptyWrite.status == Status::Ok && emptyWrite.written == 14 &&
	      memory.writes == writes + 1 && get(out, 9, 4) == 0 && out[13] == std::byte{1});
	// A size larger than the wire payload is capped before reaching a provider.
	std::vector<std::byte> large(70000, std::byte{0});
	request.fill(std::byte{0});
	request[0] = std::byte{3};
	CHECK(process(wrongFs.view(), Input{request}.first(13), large).status == Status::InternalError);
	for (unsigned op = 1; op <= 5; ++op) {
		request.fill(std::byte{0});
		request[0] = std::byte(op);
		const std::size_t fixed = op == 1 ? 9 : op == 2 ? 5 : op == 3 ? 13 : op == 4 ? 16 : 13;
		for (std::size_t n = 0; n < fixed; ++n) {
			CHECK(process(view, Input{request}.first(n), out).status == Status::InvalidData);
		}
	}
	// Arbitrary complete packets and every small reply extent, with guard bytes.
	// Run under sanitizers too: malformed lengths must never reach a span read.
	std::uint32_t seed = 91;
	for (unsigned trial = 0; trial < 4000; ++trial) {
		for (auto& b : request) {
			seed = seed * 1664525u + 1013904223u;
			b = std::byte(seed >> 24);
		}
		if (trial % 2 == 0) {
			request[0] = std::byte(1 + trial % 4);
		}
		const auto inSize = trial % 65, outSize = trial % 32;
		out.fill(std::byte{0xa5});
		r = process(view, Input{request}.first(inSize), Output{out}.subspan(1, outSize));
		CHECK(r.written <= outSize && out[0] == std::byte{0xa5} &&
		      out[1 + outSize] == std::byte{0xa5});
	}
	std::printf("Resource core/protocol: %d checks passed\n", checks);
}

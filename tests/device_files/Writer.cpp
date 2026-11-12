// FileWriter and StagedFile against the real Resource protocol handler,
// without a network: every way a write can end, deterministically. MIT.
#include <integration/files/Client.hpp>
#include <resource/BytesFile.hpp>
#include <resource/FileSystem.hpp>
#include <resource/StagedFile.hpp>
#include <resource/protocol/Protocol.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <vector>
namespace files = telemetry::integration::files;
namespace cl = telemetry::client;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Device files writer check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

// Writes in place, taking at most half of each piece (and at least one byte):
// a provider that consumes partly, with byte-offset cursors.
struct Halving {
	std::array<std::byte, 32> bytes{};

	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return bytes.size();
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input input,
	                                          bool final) noexcept
	{
		if (cursor > bytes.size() || input.size() > bytes.size() - cursor)
			return {resource::Status::InvalidData, cursor};
		const auto take = input.empty() ? 0 : std::max<std::size_t>(1, input.size() / 2);
		std::copy_n(input.begin(), take, bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
		const bool done = final && take == input.size() && cursor + take == bytes.size();
		return {resource::Status::Ok, cursor + take, static_cast<std::uint32_t>(take), done};
	}
};

// Takes every byte, but never says complete.
struct NeverComplete {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 8;
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input input,
	                                          bool) noexcept
	{
		// An empty piece must still move the cursor to be valid without data.
		return {resource::Status::Ok, cursor + input.size() + (input.empty() ? 1 : 0),
		        static_cast<std::uint32_t>(input.size()), false};
	}
};

// Says complete after its first piece, with bytes still to come.
struct EarlyComplete {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 8;
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input input,
	                                          bool) noexcept
	{
		return {resource::Status::Ok, cursor + input.size(), static_cast<std::uint32_t>(input.size()), true};
	}
};

// Refuses the content.
struct Refusing {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 4;
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input, bool) noexcept
	{
		return {resource::Status::InvalidData, cursor};
	}
};

// Replaces the whole file with a stream of any size (as the identity file does).
struct Replacing {
	std::vector<std::byte> bytes = std::vector<std::byte>(5);
	std::vector<std::byte> incoming;

	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return static_cast<resource::FileSize>(bytes.size());
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input input,
	                                          bool final) noexcept
	{
		if (cursor == 0)
			incoming.clear();
		incoming.insert(incoming.end(), input.begin(), input.end());
		if (final)
			bytes = incoming;
		return {resource::Status::Ok, cursor + input.size() + (input.empty() ? 1 : 0),
		        static_cast<std::uint32_t>(input.size()), final};
	}
};

std::array<std::byte, 40> stagedBytes{};
std::array<std::byte, 40> stageBuffer{};
unsigned commits = 0;
std::array<std::byte, 0> emptyBytes{};
std::array<std::byte, 0> emptyStage{};
std::array<std::byte, 6> readOnlyBytes{};
resource::StagedFile staged{stagedBytes, stageBuffer, [](void*, resource::Input bytes) noexcept {
	                            ++commits;
	                            CHECK(bytes.size() == 40);
	                            return resource::Status::Ok;
                            }};
// Refuses new bytes that begin with 0xFF, as a device validating a setting.
std::array<std::byte, 8> guardedBytes{};
std::array<std::byte, 8> guardedStage{};
resource::StagedFile guarded{guardedBytes, guardedStage, [](void*, resource::Input bytes) noexcept {
	                             return bytes[0] == std::byte{0xff} ? resource::Status::InvalidData
	                                                                : resource::Status::Ok;
                             }};
resource::StagedFile emptyStaged{emptyBytes, emptyStage};
const resource::BytesFile readOnly{readOnlyBytes};
Halving halving{};
NeverComplete neverComplete{};
EarlyComplete earlyComplete{};
Refusing refusing{};
Replacing replacing{};
const auto table = resource::filesystem(
    resource::file("/cfg/staged.bin", staged), resource::file("/cfg/empty.bin", emptyStaged),
    resource::file("/cfg/read-only.bin", readOnly), resource::file("/cfg/halving.bin", halving),
    resource::file("/cfg/never.bin", neverComplete), resource::file("/cfg/early.bin", earlyComplete),
    resource::file("/cfg/refusing.bin", refusing), resource::file("/cfg/replacing.bin", replacing),
    resource::file("/cfg/guarded.bin", guarded));
enum : resource::FileIndex { StagedIndex, EmptyIndex, ReadOnlyIndex, HalvingIndex, NeverIndex, EarlyIndex,
	                         RefusingIndex, ReplacingIndex, GuardedIndex };

// A client that queues file requests and lets the test answer each one.
class Device {
public:
	struct Pending {
		std::vector<std::byte> request;
		std::function<void(const cl::ResultView&)> callback;
		std::uint64_t sequence = 0;
	};

	cl::Status refuseNext = cl::Status::Ok;
	std::deque<Pending> pending;
	std::vector<std::vector<std::byte>> sent;
	std::uint64_t sequence = 0;

	template<class F>
	cl::Submission submitResource(cl::Input request, std::uint32_t replyLimit, F&& callback,
	                              std::uint32_t timeoutMs) noexcept
	{
		CHECK((replyLimit == resource::protocol::wire::statReplySize ||
		       replyLimit == resource::protocol::wire::writeReplySize) &&
		      timeoutMs == 30000);
		if (refuseNext != cl::Status::Ok) {
			const auto status = refuseNext;
			refuseNext = cl::Status::Ok;
			return {status, {}};
		}
		pending.push_back({{request.begin(), request.end()}, std::forward<F>(callback), ++sequence});
		sent.emplace_back(request.begin(), request.end());
		return {cl::Status::Ok, {this, sequence, 1}};
	}

	bool cancel(cl::RequestHandle handle) noexcept
	{
		for (auto it = pending.begin(); it != pending.end(); ++it) {
			if (it->sequence != handle.sequence)
				continue;
			auto cancelled = std::move(*it);
			pending.erase(it);
			cl::ResultView result{};
			result.status = cl::Status::Cancelled;
			result.operation = cl::Operation::Resource;
			cancelled.callback(result);
			return true;
		}
		return false;
	}

	void answer()
	{
		auto next = std::move(pending.front());
		pending.pop_front();
		std::vector<std::byte> reply(64);
		const auto written = resource::protocol::process(table.view(), next.request, reply).written;
		cl::ResultView result{};
		result.operation = cl::Operation::Resource;
		result.payload = cl::Input{reply.data(), written};
		next.callback(result);
	}

	void fail(cl::Status status)
	{
		auto next = std::move(pending.front());
		pending.pop_front();
		cl::ResultView result{};
		result.status = status;
		result.operation = cl::Operation::Resource;
		next.callback(result);
	}

	void garbage()
	{
		auto next = std::move(pending.front());
		pending.pop_front();
		const std::array<std::byte, 3> bytes{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
		cl::ResultView result{};
		result.operation = cl::Operation::Resource;
		result.payload = bytes;
		next.callback(result);
	}

	// The final flag and the payload length of each WRITE sent, in order.
	struct Piece {
		bool final;
		std::size_t size;
	};

	[[nodiscard]] std::vector<Piece> writes() const
	{
		std::vector<Piece> out;
		for (const auto& request : sent)
			if (!request.empty() && request[0] == std::byte(resource::protocol::Op::Write))
				out.push_back({request[13] != std::byte{}, request.size() - 16});
		return out;
	}
};

using Writer = files::FileWriter<Device, 16>;

void drive(Device& device, Writer& writer)
{
	for (unsigned turn = 0; turn < 1000 && !writer.finished(); ++turn) {
		if (!device.pending.empty())
			device.answer();
		(void)writer.poll();
	}
}

std::vector<std::byte> pattern(std::size_t size, unsigned seed)
{
	std::vector<std::byte> bytes(size);
	for (std::size_t i = 0; i < size; ++i)
		bytes[i] = std::byte((i * 7 + seed) & 0xffu);
	return bytes;
}

// The staged file on its own: reads keep the old bytes while a write is
// staged; a stream too long or too short is refused and changes nothing.
void stagedAlone()
{
	std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
	std::array<std::byte, 4> stage{};
	resource::StagedFile file{bytes, stage};
	const std::array<std::byte, 2> ab{std::byte{0xa}, std::byte{0xb}};
	CHECK(file.write(2, ab, false).status == resource::Status::InvalidCursor);
	auto result = file.write(0, ab, false);
	CHECK(result.status == resource::Status::Ok && result.consumed == 2 && result.next == 2 && !result.complete &&
	      file.staged() == 2);
	std::array<std::byte, 4> out{};
	CHECK(file.read(0, out).written == 4 && out == bytes && bytes[0] == std::byte{1});
	// Shorter than the file: refused, the old bytes kept.
	CHECK(file.write(2, {}, true).status == resource::Status::InvalidData && bytes[0] == std::byte{1} &&
	      file.staged() == 0);
	// Longer: refused, nothing staged from it.
	const std::array<std::byte, 5> five{};
	CHECK(file.write(0, five, true).status == resource::Status::InvalidData && bytes[0] == std::byte{1});
	// A new transfer drops an unfinished one.
	CHECK(file.write(0, ab, false).status == resource::Status::Ok);
	CHECK(file.write(0, ab, false).status == resource::Status::Ok);
	result = file.write(2, ab, true);
	CHECK(result.status == resource::Status::Ok && result.complete && bytes[0] == std::byte{0xa} &&
	      bytes[3] == std::byte{0xb} && file.staged() == 0);
	// A continuation with nothing in it and no end is no progress: refused.
	CHECK(file.write(0, ab, false).status == resource::Status::Ok &&
	      file.write(2, {}, false).status == resource::Status::InvalidData);
}

void complete()
{
	Device device;
	Writer writer{device};
	const auto bytes = pattern(40, 3);
	CHECK(writer.start(StagedIndex, bytes) == cl::Status::Ok && writer.inFlight());
	drive(device, writer);
	CHECK(writer.state() == files::WriteState::Complete && writer.written() == 40 && writer.total() == 40 &&
	      writer.size() == 40);
	CHECK(std::equal(bytes.begin(), bytes.end(), stagedBytes.begin()) && commits == 1);
	// Pieces of 16, 16 and 8; final on the last only.
	const auto pieces = device.writes();
	CHECK(pieces.size() == 3 && !pieces[0].final && !pieces[1].final && pieces[2].final &&
	      pieces[0].size == 16 && pieces[1].size == 16 && pieces[2].size == 8);
	// A smaller piece size given at the start.
	Device again;
	Writer small{again};
	const auto other = pattern(40, 9);
	CHECK(small.start(StagedIndex, other, 10) == cl::Status::Ok);
	drive(again, small);
	CHECK(small.state() == files::WriteState::Complete && again.writes().size() == 4 && commits == 2 &&
	      std::equal(other.begin(), other.end(), stagedBytes.begin()));
	// An empty file: one empty final piece.
	Device empty;
	Writer nothing{empty};
	CHECK(nothing.start(EmptyIndex, {}) == cl::Status::Ok);
	drive(empty, nothing);
	CHECK(nothing.state() == files::WriteState::Complete && empty.writes().size() == 1 &&
	      empty.writes()[0].final && empty.writes()[0].size == 0);
	// A provider that replaces the whole file takes another size when allowed.
	Device replace;
	Writer whole{replace};
	const auto seven = pattern(7, 1);
	CHECK(whole.start(ReplacingIndex, seven, 16, true) == cl::Status::Ok);
	drive(replace, whole);
	CHECK(whole.state() == files::WriteState::Complete && replacing.bytes.size() == 7);
}

// A device that takes part of a piece: the rest is sent from the cursor it
// returned, still final when it was.
void partial()
{
	Device device;
	Writer writer{device};
	const auto bytes = pattern(32, 5);
	CHECK(writer.start(HalvingIndex, bytes) == cl::Status::Ok);
	drive(device, writer);
	CHECK(writer.state() == files::WriteState::Complete && writer.written() == 32 &&
	      std::equal(bytes.begin(), bytes.end(), halving.bytes.begin()));
	const auto pieces = device.writes();
	// 16 -> 8 taken, then 8 -> 4, ...; every piece sent once the first 16 are
	// taken carries final only when it reaches the end.
	CHECK(pieces.size() > 4 && pieces.front().size == 16 && !pieces.front().final && pieces.back().final);
	// Once a piece is final, every resend of its rest is final too.
	bool finalOnlyAtEnd = true;
	for (std::size_t i = 0; i + 1 < pieces.size(); ++i)
		finalOnlyAtEnd = finalOnlyAtEnd && (!pieces[i].final || pieces[i + 1].final);
	CHECK(finalOnlyAtEnd);
}

void endings()
{
	// Not writable, another size, refused content, a reply that is not one.
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(6, 0);
		CHECK(writer.start(ReadOnlyIndex, bytes) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::NotWritable && device.writes().empty());
	}
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(39, 0);
		CHECK(writer.start(StagedIndex, bytes) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::SizeMismatch && writer.size() == 40 && device.writes().empty());
	}
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(4, 0);
		CHECK(writer.start(RefusingIndex, bytes) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::Refused &&
		      writer.deviceStatus() == resource::Status::InvalidData && writer.written() == 0);
	}
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(40, 0);
		CHECK(writer.start(StagedIndex, bytes) == cl::Status::Ok);
		device.answer();
		(void)writer.poll();
		device.garbage();
		CHECK(writer.finished() && writer.state() == files::WriteState::Malformed);
	}
	// The device's commit refuses the new bytes: Refused with its status, the
	// old bytes kept; a write it accepts then goes in.
	{
		Device device;
		Writer writer{device};
		auto bad = pattern(8, 0);
		bad[0] = std::byte{0xff};
		CHECK(writer.start(GuardedIndex, bad) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::Refused &&
		      writer.deviceStatus() == resource::Status::InvalidData &&
		      std::all_of(guardedBytes.begin(), guardedBytes.end(), [](std::byte b) { return b == std::byte{}; }));
		const auto good = pattern(8, 1);
		Writer again{device};
		CHECK(again.start(GuardedIndex, good) == cl::Status::Ok);
		drive(device, again);
		CHECK(again.state() == files::WriteState::Complete &&
		      std::equal(good.begin(), good.end(), guardedBytes.begin()));
	}
	// Complete before every byte: malformed.
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(8, 0);
		CHECK(writer.start(EarlyIndex, bytes, 4) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::Malformed && writer.written() == 4);
	}
	// Everything taken, never complete: empty final pieces, a bounded number.
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(8, 0);
		CHECK(writer.start(NeverIndex, bytes) == cl::Status::Ok);
		drive(device, writer);
		const auto pieces = device.writes();
		CHECK(writer.state() == files::WriteState::Stalled && writer.written() == 8 && pieces.size() > 2 &&
		      pieces.size() < 40 && pieces.back().final && pieces.back().size == 0);
	}
	// A timeout ends it as Failed and nothing is sent again.
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(40, 0);
		CHECK(writer.start(StagedIndex, bytes) == cl::Status::Ok);
		device.answer();
		(void)writer.poll();
		const auto before = device.sent.size();
		device.fail(cl::Status::Timeout);
		for (int turn = 0; turn < 5; ++turn)
			(void)writer.poll();
		CHECK(writer.finished() && writer.state() == files::WriteState::Failed &&
		      writer.clientStatus() == cl::Status::Timeout && device.sent.size() == before && device.pending.empty());
	}
}

// Cancel between pieces: the staged file keeps its old bytes; a later write
// from the start replaces the unfinished stage.
void cancelling()
{
	Device device;
	Writer writer{device};
	const auto old = std::vector<std::byte>(stagedBytes.begin(), stagedBytes.end());
	const auto bytes = pattern(40, 11);
	CHECK(writer.start(StagedIndex, bytes) == cl::Status::Ok);
	device.answer();  // Stat
	(void)writer.poll();
	device.answer();  // the first piece
	writer.cancel();
	CHECK(writer.finished() && writer.state() == files::WriteState::Cancelled && writer.written() == 16);
	CHECK(std::equal(old.begin(), old.end(), stagedBytes.begin()) && staged.staged() == 16);
	// Cancelled while a piece waits in the queue: the queue cancels it.
	Writer queued{device};
	CHECK(queued.start(StagedIndex, bytes) == cl::Status::Ok && queued.inFlight());
	queued.cancel();
	CHECK(queued.finished() && queued.state() == files::WriteState::Cancelled);
	// A write from the start completes over the abandoned stage.
	Writer fresh{device};
	CHECK(fresh.start(StagedIndex, bytes) == cl::Status::Ok);
	drive(device, fresh);
	CHECK(fresh.state() == files::WriteState::Complete && std::equal(bytes.begin(), bytes.end(), stagedBytes.begin()));
}

void submissions()
{
	Device device;
	const auto bytes = pattern(40, 2);
	// A busy queue leaves the write due; the next poll submits it.
	Writer busy{device};
	device.refuseNext = cl::Status::Busy;
	CHECK(busy.start(StagedIndex, bytes) == cl::Status::Busy && busy.state() == files::WriteState::Running && busy.due());
	CHECK(busy.poll() == cl::Status::Ok && busy.inFlight());
	drive(device, busy);
	CHECK(busy.state() == files::WriteState::Complete);
	// Any other refusal ends it; a second start while running is Busy.
	Writer refused{device};
	device.refuseNext = cl::Status::NotAdmitted;
	CHECK(refused.start(StagedIndex, bytes) == cl::Status::NotAdmitted && refused.finished() &&
	      refused.state() == files::WriteState::Failed && refused.clientStatus() == cl::Status::NotAdmitted);
	Writer twice{device};
	CHECK(twice.start(StagedIndex, bytes) == cl::Status::Ok && twice.start(StagedIndex, bytes) == cl::Status::Busy);
	drive(device, twice);
	CHECK(twice.state() == files::WriteState::Complete && twice.start(StagedIndex, bytes, 0) == cl::Status::InvalidArgument);
	// One request on its own: a piece the caller builds in its scratch.
	std::array<std::byte, 16 + 4> scratch{};
	const std::array<std::byte, 4> four{std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
	bool done = false;
	CHECK(files::write(device, scratch, RefusingIndex, 0, four, true, [&](const files::WriteResult& result) noexcept {
		CHECK(result.parsed && result.reply.status == resource::Status::InvalidData && !result.succeeded());
		done = true;
	}));
	device.answer();
	CHECK(done);
	// A scratch too small for the piece is refused before anything is sent.
	std::array<std::byte, 10> tiny{};
	CHECK(files::write(device, tiny, RefusingIndex, 0, four, true, [](const files::WriteResult&) noexcept {}).status ==
	      cl::Status::InvalidArgument);
}
} // namespace

int main()
{
	stagedAlone();
	complete();
	partial();
	endings();
	cancelling();
	submissions();
	std::printf("Device files writer: %u checks, 0 failures\n", checks);
	return 0;
}

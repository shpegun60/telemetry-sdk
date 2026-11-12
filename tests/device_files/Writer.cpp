// FileWriter and files' write() and save() against the real Resource protocol handler,
// without a network: every way a write can end, deterministically. MIT.
#include <integration/files/Client.hpp>
#include <resource/BytesFile.hpp>
#include <resource/FileSystem.hpp>
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

// Moves its cursor without ever taking a byte.
struct Drifting {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 8;
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input,
	                                          bool) noexcept
	{
		return {resource::Status::Ok, cursor + 1, 0, false};
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
		return {resource::Status::Ok, cursor + input.size(),
		        static_cast<std::uint32_t>(input.size()), true};
	}
};

// Refuses the content.
struct Refusing {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 4;
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input,
	                                          bool) noexcept
	{
		return {resource::Status::InvalidData, cursor};
	}
};

// N bytes to read and write: the pieces of a write are kept apart and the
// file changes only when the final piece completes it (byte-offset cursors);
// unless they begin with 0xFF when guarded - as a device validating a setting.
template<std::size_t N>
struct Store {
	std::array<std::byte, N> bytes{};
	std::array<std::byte, N> incoming{};
	std::size_t taken = 0;
	unsigned applied = 0;
	bool guarded = false;

	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return N;
	}

	[[nodiscard]] resource::ReadResult read(resource::Cursor cursor,
	                                        resource::Output output) const noexcept
	{
		return resource::BytesFile{bytes}.read(cursor, output);
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input in,
	                                          bool final) noexcept
	{
		if (cursor == 0)
			taken = 0;
		if (cursor != taken || in.size() > N - taken)
			return {resource::Status::InvalidCursor, cursor};
		if constexpr (N > 0)
			std::copy(in.begin(), in.end(), incoming.begin() + static_cast<std::ptrdiff_t>(taken));
		taken += in.size();
		if (!final)
			return in.empty() ? resource::WriteResult{resource::Status::InvalidData, cursor}
			                  : resource::WriteResult{resource::Status::Ok, taken,
			                                          static_cast<std::uint32_t>(in.size())};
		if (taken != N || (guarded && N > 0 && incoming[0] == std::byte{0xff}))
			return {resource::Status::InvalidData, cursor};
		bytes = incoming;
		++applied;
		taken = 0;
		return {resource::Status::Ok, N, static_cast<std::uint32_t>(in.size()), true};
	}

	[[nodiscard]] std::size_t staged() const noexcept
	{
		return taken;
	}
};

Store<40> stagedStore;
Store<8> guardedStore{{}, {}, 0, 0, true};
Store<0> emptyStore;

// Written as a Store, and persisted on SAVE: at once, or refusing.
struct Persisting : Store<8> {
	unsigned persisted = 0;
	resource::Status refuse = resource::Status::Ok;

	[[nodiscard]] resource::SaveResult save(resource::Cursor cursor) noexcept
	{
		if (refuse != resource::Status::Ok)
			return {refuse, cursor};
		++persisted;
		return {resource::Status::Ok, cursor, true};
	}
};

Persisting persisting;

// Keeps its bytes in three steps, as slow flash would.
struct Stepwise : Store<8> {
	unsigned steps = 0;

	[[nodiscard]] resource::SaveResult save(resource::Cursor cursor) noexcept
	{
		++steps;
		return cursor < 2 ? resource::SaveResult{resource::Status::Ok, cursor + 1, false}
		                  : resource::SaveResult{resource::Status::Ok, cursor, true};
	}
};

Stepwise stepwise;

// Moves its cursor on every SAVE and never completes.
struct Endless : Store<8> {
	unsigned steps = 0;

	[[nodiscard]] resource::SaveResult save(resource::Cursor cursor) noexcept
	{
		++steps;
		return {resource::Status::Ok, cursor + 1, false};
	}
};

Endless endless;
Drifting drifting{};
std::array<std::byte, 6> readOnlyBytes{};
const resource::BytesFile readOnly{readOnlyBytes};
Halving halving{};
NeverComplete neverComplete{};
EarlyComplete earlyComplete{};
Refusing refusing{};
const auto table = resource::filesystem(
    resource::file("/cfg/staged.bin", stagedStore), resource::file("/cfg/empty.bin", emptyStore),
    resource::file("/cfg/read-only.bin", readOnly), resource::file("/cfg/halving.bin", halving),
    resource::file("/cfg/never.bin", neverComplete),
    resource::file("/cfg/early.bin", earlyComplete), resource::file("/cfg/refusing.bin", refusing),
    resource::file("/cfg/guarded.bin", guardedStore),
    resource::file("/cfg/persisting.bin", persisting),
    resource::file("/cfg/stepwise.bin", stepwise), resource::file("/cfg/endless.bin", endless),
    resource::file("/cfg/drifting.bin", drifting));

enum : resource::FileIndex {
	StagedIndex,
	EmptyIndex,
	ReadOnlyIndex,
	HalvingIndex,
	NeverIndex,
	EarlyIndex,
	RefusingIndex,
	GuardedIndex,
	PersistingIndex,
	StepwiseIndex,
	EndlessIndex,
	DriftingIndex
};

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
		       replyLimit == resource::protocol::wire::writeReplySize ||
		       replyLimit == resource::protocol::wire::saveReplySize) &&
		      timeoutMs == 30000);
		if (refuseNext != cl::Status::Ok) {
			const auto status = refuseNext;
			refuseNext = cl::Status::Ok;
			return {status, {}};
		}
		pending.push_back(
		    {{request.begin(), request.end()}, std::forward<F>(callback), ++sequence});
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

	void shortError()
	{
		auto next = std::move(pending.front());
		pending.pop_front();
		const std::array<std::byte, 1> bytes{std::byte(resource::Status::InvalidData)};
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

	// SAVE requests sent.
	[[nodiscard]] std::size_t saves() const
	{
		return static_cast<std::size_t>(
		    std::count_if(sent.begin(), sent.end(), [](const auto& request) {
			    return !request.empty() && request[0] == std::byte(resource::protocol::Op::Save);
		    }));
	}
};

using Writer = files::FileWriter<Device, 16>;

void drive(Device& device, Writer& writer, unsigned turns = 1000)
{
	for (unsigned turn = 0; turn < turns && !writer.finished(); ++turn) {
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

void complete()
{
	Device device;
	Writer writer{device};
	const auto bytes = pattern(40, 3);
	CHECK(writer.start(StagedIndex, bytes) == cl::Status::Ok && writer.inFlight());
	drive(device, writer);
	CHECK(writer.state() == files::WriteState::Complete && writer.written() == 40 &&
	      writer.total() == 40 && writer.size() == 40);
	CHECK(std::equal(bytes.begin(), bytes.end(), stagedStore.bytes.begin()) &&
	      stagedStore.applied == 1);
	// Pieces of 16, 16 and 8, final on the last only; writing never saves.
	const auto pieces = device.writes();
	CHECK(pieces.size() == 3 && !pieces[0].final && !pieces[1].final && pieces[2].final &&
	      pieces[0].size == 16 && pieces[1].size == 16 && pieces[2].size == 8 &&
	      device.saves() == 0);
	// A smaller piece size given at the start.
	Device again;
	Writer small{again};
	const auto other = pattern(40, 9);
	CHECK(small.start(StagedIndex, other, 10) == cl::Status::Ok);
	drive(again, small);
	CHECK(small.state() == files::WriteState::Complete && again.writes().size() == 4 &&
	      stagedStore.applied == 2 &&
	      std::equal(other.begin(), other.end(), stagedStore.bytes.begin()));
	// An empty file: one empty final piece.
	Device empty;
	Writer nothing{empty};
	CHECK(nothing.start(EmptyIndex, {}) == cl::Status::Ok);
	drive(empty, nothing);
	CHECK(nothing.state() == files::WriteState::Complete && empty.writes().size() == 1 &&
	      empty.writes()[0].final && empty.writes()[0].size == 0);
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
	// 16 -> 8 taken, then the rest from the returned cursor; once a piece is
	// final, every resend of its rest is final too.
	CHECK(pieces.size() > 4 && pieces.front().size == 16 && !pieces.front().final &&
	      pieces.back().final);
	bool finalOnlyAtEnd = true;
	for (std::size_t i = 0; i + 1 < pieces.size(); ++i)
		finalOnlyAtEnd = finalOnlyAtEnd && (!pieces[i].final || pieces[i + 1].final);
	CHECK(finalOnlyAtEnd);
}

// Saving is a run of its own: written first, then Stat and SAVE until
// complete. A refusal is Refused with its status; a file that cannot save is
// NotWritable by its Stat, and no SAVE is sent.
void saving()
{
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(8, 4);
		CHECK(writer.start(PersistingIndex, bytes) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::Complete && device.saves() == 0 &&
		      persisting.persisted == 0 &&
		      std::equal(bytes.begin(), bytes.end(), persisting.bytes.begin()));
		CHECK(writer.save(PersistingIndex) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::Complete && device.saves() == 1 &&
		      persisting.persisted == 1 && writer.total() == 0 && writer.size() == 8);
		// In steps: SAVE again from each returned cursor until complete.
		CHECK(writer.save(StepwiseIndex) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::Complete && stepwise.steps == 3 &&
		      device.saves() == 4);
	}
	{
		Device device;
		Writer writer{device};
		persisting.refuse = resource::Status::InternalError;
		CHECK(writer.save(PersistingIndex) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::Refused &&
		      writer.deviceStatus() == resource::Status::InternalError &&
		      persisting.persisted == 1);
		persisting.refuse = resource::Status::Ok;
		const auto before = device.saves();
		CHECK(writer.save(StagedIndex) == cl::Status::Ok);
		drive(device, writer);
		CHECK(writer.state() == files::WriteState::NotWritable && device.saves() == before);
	}
	// A save that never completes: bounded steps, then Stalled.
	{
		Device device;
		Writer writer{device};
		CHECK(writer.save(EndlessIndex) == cl::Status::Ok);
		drive(device, writer, 10000);
		CHECK(writer.state() == files::WriteState::Stalled && endless.steps == 4097 &&
		      device.saves() == 4097);
	}
	// In the middle of a save: a reply that is not one, the one-byte
	// InvalidData, a timeout, and a cancel while its SAVE waits in the queue.
	const auto interrupted = [](auto&& interrupt, files::WriteState expected) {
		Device device;
		Writer writer{device};
		stepwise.steps = 0;
		CHECK(writer.save(StepwiseIndex) == cl::Status::Ok);
		device.answer(); // Stat
		(void)writer.poll();
		device.answer(); // the first step
		(void)writer.poll();
		CHECK(device.pending.size() == 1 && stepwise.steps == 1);
		interrupt(device, writer);
		for (int turn = 0; turn < 5; ++turn)
			(void)writer.poll();
		CHECK(writer.finished() && writer.state() == expected && device.pending.empty() &&
		      device.saves() == 2);
		return writer.deviceStatus();
	};
	interrupted(
	    [](Device& device, Writer&) {
		    device.garbage();
	    },
	    files::WriteState::Malformed);
	CHECK(interrupted(
	          [](Device& device, Writer&) {
		          device.shortError();
	          },
	          files::WriteState::Refused) == resource::Status::InvalidData);
	interrupted(
	    [](Device& device, Writer&) {
		    device.fail(cl::Status::Timeout);
	    },
	    files::WriteState::Failed);
	interrupted(
	    [](Device&, Writer& writer) {
		    writer.cancel();
	    },
	    files::WriteState::Cancelled);
	// One SAVE on its own.
	Device device;
	bool done = false;
	CHECK(files::save(device, PersistingIndex, 0, [&](const files::SaveResult& result) noexcept {
		CHECK(result.succeeded() && result.reply.complete);
		done = true;
	}));
	device.answer();
	CHECK(done && persisting.persisted == 2);
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
		CHECK(writer.state() == files::WriteState::SizeMismatch && writer.size() == 40 &&
		      device.writes().empty());
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
		      std::all_of(guardedStore.bytes.begin(), guardedStore.bytes.end(), [](std::byte b) {
			      return b == std::byte{};
		      }));
		const auto good = pattern(8, 1);
		Writer again{device};
		CHECK(again.start(GuardedIndex, good) == cl::Status::Ok);
		drive(device, again);
		CHECK(again.state() == files::WriteState::Complete &&
		      std::equal(good.begin(), good.end(), guardedStore.bytes.begin()));
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
		CHECK(writer.state() == files::WriteState::Stalled && writer.written() == 8 &&
		      pieces.size() > 2 && pieces.size() < 40 && pieces.back().final &&
		      pieces.back().size == 0);
	}
	// The cursor moves, no byte is ever taken: bounded, then Stalled.
	{
		Device device;
		Writer writer{device};
		const auto bytes = pattern(8, 0);
		CHECK(writer.start(DriftingIndex, bytes) == cl::Status::Ok);
		drive(device, writer, 10000);
		CHECK(writer.state() == files::WriteState::Stalled && writer.written() == 0 &&
		      device.writes().size() == 4097);
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
		      writer.clientStatus() == cl::Status::Timeout && device.sent.size() == before &&
		      device.pending.empty());
	}
}

// Cancel between pieces: the staged file keeps its old bytes; a later write
// from the start replaces the unfinished stage.
void cancelling()
{
	Device device;
	Writer writer{device};
	const auto old = std::vector<std::byte>(stagedStore.bytes.begin(), stagedStore.bytes.end());
	const auto bytes = pattern(40, 11);
	CHECK(writer.start(StagedIndex, bytes) == cl::Status::Ok);
	device.answer(); // Stat
	(void)writer.poll();
	device.answer(); // the first piece
	writer.cancel();
	CHECK(writer.finished() && writer.state() == files::WriteState::Cancelled &&
	      writer.written() == 16);
	CHECK(std::equal(old.begin(), old.end(), stagedStore.bytes.begin()) &&
	      stagedStore.staged() == 16);
	// Cancelled while a piece waits in the queue: the queue cancels it.
	Writer queued{device};
	CHECK(queued.start(StagedIndex, bytes) == cl::Status::Ok && queued.inFlight());
	queued.cancel();
	CHECK(queued.finished() && queued.state() == files::WriteState::Cancelled);
	// A write from the start completes over the abandoned stage.
	Writer fresh{device};
	CHECK(fresh.start(StagedIndex, bytes) == cl::Status::Ok);
	drive(device, fresh);
	CHECK(fresh.state() == files::WriteState::Complete &&
	      std::equal(bytes.begin(), bytes.end(), stagedStore.bytes.begin()));
}

void submissions()
{
	Device device;
	const auto bytes = pattern(40, 2);
	// A busy queue leaves the write due; the next poll submits it.
	Writer busy{device};
	device.refuseNext = cl::Status::Busy;
	CHECK(busy.start(StagedIndex, bytes) == cl::Status::Busy &&
	      busy.state() == files::WriteState::Running && busy.due());
	CHECK(busy.poll() == cl::Status::Ok && busy.inFlight());
	drive(device, busy);
	CHECK(busy.state() == files::WriteState::Complete);
	// Any other refusal ends it; a second start while running is Busy.
	Writer refused{device};
	device.refuseNext = cl::Status::NotAdmitted;
	CHECK(refused.start(StagedIndex, bytes) == cl::Status::NotAdmitted && refused.finished() &&
	      refused.state() == files::WriteState::Failed &&
	      refused.clientStatus() == cl::Status::NotAdmitted);
	Writer twice{device};
	CHECK(twice.start(StagedIndex, bytes) == cl::Status::Ok &&
	      twice.start(StagedIndex, bytes) == cl::Status::Busy);
	drive(device, twice);
	CHECK(twice.state() == files::WriteState::Complete &&
	      twice.start(StagedIndex, bytes, 0) == cl::Status::InvalidArgument);
	// One request on its own: a piece the caller builds in its scratch.
	std::array<std::byte, 16 + 4> scratch{};
	const std::array<std::byte, 4> four{std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
	bool done = false;
	CHECK(files::write(device, scratch, RefusingIndex, 0, four, true,
	                   [&](const files::WriteResult& result) noexcept {
		                   CHECK(result.parsed &&
		                         result.reply.status == resource::Status::InvalidData &&
		                         !result.succeeded());
		                   done = true;
	                   }));
	device.answer();
	CHECK(done);
	// A scratch too small for the piece is refused before anything is sent.
	std::array<std::byte, 10> tiny{};
	CHECK(files::write(device, tiny, RefusingIndex, 0, four, true,
	                   [](const files::WriteResult&) noexcept {
	                   })
	          .status == cl::Status::InvalidArgument);
}
} // namespace

int main()
{
	complete();
	partial();
	saving();
	endings();
	cancelling();
	submissions();
	std::printf("Device files writer: %u checks, 0 failures\n", checks);
	return 0;
}

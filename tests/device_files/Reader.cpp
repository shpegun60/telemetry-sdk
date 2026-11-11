// FileReader and the files requests against the real Resource protocol
// handler, without a network: every way a read can end, deterministically. MIT.
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
#include <string>
#include <vector>
namespace files = telemetry::integration::files;
namespace cl = telemetry::client;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Device files reader check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

// Stat says 10 bytes; reads deliver 25 in pieces of at most 7: a growing log.
struct Growing {
	static constexpr resource::FileSize total = 25;

	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 10;
	}

	[[nodiscard]] resource::ReadResult read(resource::Cursor cursor,
	                                        resource::Output output) const noexcept
	{
		if (cursor > total)
			return {resource::Status::InvalidCursor, cursor};
		if (cursor == total)
			return {resource::Status::Ok, cursor, 0, true};
		const auto count = std::min<resource::Cursor>({output.size(), total - cursor, 7});
		for (resource::Cursor i = 0; i < count; ++i)
			output[i] = std::byte(cursor + i);
		return {resource::Status::Ok, cursor + count, static_cast<std::uint32_t>(count), false};
	}
};

// Answers without data and without moving its cursor.
struct Stuck {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 4;
	}

	[[nodiscard]] resource::ReadResult read(resource::Cursor cursor,
	                                        resource::Output) const noexcept
	{
		return {resource::Status::Ok, cursor, 0, false};
	}
};

struct WriteOnly {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 4;
	}

	[[nodiscard]] resource::WriteResult write(resource::Cursor, resource::Input, bool) noexcept
	{
		return {resource::Status::Ok, 0, 0, true};
	}
};

std::array<std::byte, 50> patternBytes{};
std::array<std::byte, 0> emptyBytes{};
const resource::BytesFile patternFile{patternBytes};
const resource::BytesFile emptyFile{emptyBytes};
const Growing growing{};
const Stuck stuck{};
WriteOnly writeOnly{};
const auto table = resource::filesystem(
    resource::file("/data/pattern.bin", patternFile), resource::file("/data/empty.csv", emptyFile),
    resource::file("/logs/growing.log", growing), resource::file("/logs/stuck.bin", stuck),
    resource::file("/cfg/write-only.bin", writeOnly));

// A client that queues file requests and lets the test answer each one.
class Device {
public:
	struct Pending {
		std::vector<std::byte> request;
		std::function<void(const cl::ResultView&)> callback;
		std::uint64_t sequence = 0;
	};

	std::size_t replyBytes = 12 + 16;
	cl::Status refuseNext = cl::Status::Ok;
	std::deque<Pending> pending;
	std::uint64_t sequence = 0;

	template<class F>
	cl::Submission submitResource(cl::Input request, std::uint32_t replyLimit, F&& callback,
	                              std::uint32_t timeoutMs) noexcept
	{
		CHECK(replyLimit == files::maxReplyBytes && timeoutMs == 30000);
		if (refuseNext != cl::Status::Ok) {
			const auto status = refuseNext;
			refuseNext = cl::Status::Ok;
			return {status, {}};
		}
		pending.push_back(
		    {{request.begin(), request.end()}, std::forward<F>(callback), ++sequence});
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
		std::vector<std::byte> reply(replyBytes);
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

	// A well-formed Read reply, not at the end, with the cursor left where it
	// was - with no data or with some: what a misbehaving device could send.
	void noProgress(resource::Cursor cursor, std::uint16_t count = 0)
	{
		auto next = std::move(pending.front());
		pending.pop_front();
		std::vector<std::byte> bytes(12 + count, std::byte{0x33});
		bytes[0] = std::byte{0};
		bytes[9] = std::byte{0};
		bytes[10] = std::byte(count & 0xffu);
		bytes[11] = std::byte(count >> 8);
		for (unsigned i = 0; i < 8; ++i)
			bytes[1 + i] = std::byte((cursor >> (8 * i)) & 0xffu);
		cl::ResultView result{};
		result.operation = cl::Operation::Resource;
		result.payload = bytes;
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
};

struct Collected {
	std::vector<std::byte> bytes;
	bool stopAfterFirst = false;
};

struct Sink {
	Collected* into;

	bool operator()(cl::Input piece) noexcept
	{
		into->bytes.insert(into->bytes.end(), piece.begin(), piece.end());
		return !into->stopAfterFirst;
	}
};

using Reader = files::FileReader<Device, Sink>;

// Answers until the reader ends, polling it as an owner loop would.
void drive(Device& device, Reader& reader)
{
	for (unsigned turn = 0; turn < 1000 && !reader.finished(); ++turn) {
		if (!device.pending.empty())
			device.answer();
		(void)reader.poll();
	}
	CHECK(reader.finished() && device.pending.empty());
}

void listing()
{
	Device device;
	device.replyBytes = 4096;
	std::vector<std::string> paths;
	bool done = false;
	CHECK(files::list(device, 0, [&](const files::ListResult& result) noexcept {
		CHECK(result.succeeded() && result.reply.eof && result.reply.next == 5);
		for (const auto path : result.reply.paths)
			paths.emplace_back(path);
		done = true;
	}));
	device.answer();
	CHECK(done);
	const std::vector<std::string> expected{"/data/pattern.bin", "/data/empty.csv",
	                                        "/logs/growing.log", "/logs/stuck.bin",
	                                        "/cfg/write-only.bin"};
	CHECK(paths == expected);
	bool statted = false;
	CHECK(files::stat(device, 4, [&](const files::StatResult& result) noexcept {
		CHECK(result.succeeded() && result.reply.size == 4 &&
		      !resource::has(result.reply.flags, resource::FileFlag::Readable) &&
		      resource::has(result.reply.flags, resource::FileFlag::Writable));
		statted = true;
	}));
	device.answer();
	CHECK(statted);
}

void complete()
{
	for (std::size_t i = 0; i < patternBytes.size(); ++i)
		patternBytes[i] = std::byte(i * 13 + 1);
	Device device;
	Collected collected;
	Reader reader{device, Sink{&collected}};
	CHECK(reader.state() == files::ReadState::Idle && !reader.finished());
	CHECK(reader.start(0) == cl::Status::Ok && reader.inFlight());
	CHECK(reader.start(0) == cl::Status::Busy); // One read at a time.
	drive(device, reader);
	CHECK(reader.state() == files::ReadState::Complete && reader.size() == 50 &&
	      reader.received() == 50);
	CHECK(collected.bytes.size() == 50 &&
	      std::equal(patternBytes.begin(), patternBytes.end(), collected.bytes.begin()));
	CHECK(device.sequence == 1 + 4); // Stat, then four pieces of at most 16 bytes.

	Collected none;
	Reader empty{device, Sink{&none}};
	CHECK(empty.start(1) == cl::Status::Ok);
	drive(device, empty);
	CHECK(empty.state() == files::ReadState::Complete && empty.received() == 0 &&
	      none.bytes.empty());

	// A file that grows while it is read ends where the device says it ends.
	Collected log;
	Reader grown{device, Sink{&log}};
	CHECK(grown.start(2) == cl::Status::Ok);
	drive(device, grown);
	CHECK(grown.state() == files::ReadState::Complete && grown.size() == 10 &&
	      grown.received() == 25);
	CHECK(log.bytes.size() == 25 && log.bytes[24] == std::byte{24});

	// A reader is reusable once finished.
	collected.bytes.clear();
	CHECK(reader.start(0) == cl::Status::Ok);
	drive(device, reader);
	CHECK(reader.state() == files::ReadState::Complete && collected.bytes.size() == 50);
}

void endings()
{
	Device device;
	Collected collected;

	// The SDK's own server refuses a provider that answers without progress.
	Reader stuckReader{device, Sink{&collected}};
	CHECK(stuckReader.start(3) == cl::Status::Ok);
	drive(device, stuckReader);
	CHECK(stuckReader.state() == files::ReadState::Refused);

	// Another device might send it anyway; the reader stops on its own.
	Reader stalled{device, Sink{&collected}};
	CHECK(stalled.start(0) == cl::Status::Ok);
	device.answer();
	CHECK(stalled.poll() == cl::Status::Ok && stalled.inFlight());
	device.noProgress(0);
	CHECK(stalled.finished() && stalled.state() == files::ReadState::Stalled);

	// Data without a moving cursor would repeat forever; nothing reaches the sink.
	Collected repeated;
	Reader looping{device, Sink{&repeated}};
	CHECK(looping.start(0) == cl::Status::Ok);
	device.answer();
	CHECK(looping.poll() == cl::Status::Ok && looping.inFlight());
	device.noProgress(0, 5);
	CHECK(looping.finished() && looping.state() == files::ReadState::Malformed &&
	      repeated.bytes.empty());

	Reader writeOnlyReader{device, Sink{&collected}};
	CHECK(writeOnlyReader.start(4) == cl::Status::Ok);
	drive(device, writeOnlyReader);
	CHECK(writeOnlyReader.state() == files::ReadState::NotReadable);

	Reader missing{device, Sink{&collected}};
	CHECK(missing.start(99) == cl::Status::Ok);
	drive(device, missing);
	CHECK(missing.state() == files::ReadState::Refused &&
	      missing.deviceStatus() != resource::Status::Ok);

	Collected first;
	first.stopAfterFirst = true;
	Reader stopped{device, Sink{&first}};
	CHECK(stopped.start(0) == cl::Status::Ok);
	drive(device, stopped);
	CHECK(stopped.state() == files::ReadState::SinkStopped && first.bytes.size() == 16 &&
	      stopped.received() == 0);

	Reader timedOut{device, Sink{&collected}};
	CHECK(timedOut.start(0) == cl::Status::Ok);
	device.answer();
	CHECK(timedOut.poll() == cl::Status::Ok && timedOut.inFlight());
	device.fail(cl::Status::Timeout);
	CHECK(timedOut.finished() && timedOut.state() == files::ReadState::Failed &&
	      timedOut.clientStatus() == cl::Status::Timeout);

	Reader broken{device, Sink{&collected}};
	CHECK(broken.start(0) == cl::Status::Ok);
	device.garbage();
	CHECK(broken.finished() && broken.state() == files::ReadState::Malformed);
}

void cancelling()
{
	Device device;
	Collected collected;

	// Cancel while a piece waits in the queue: the queue cancels it at once.
	Reader inFlight{device, Sink{&collected}};
	CHECK(inFlight.start(0) == cl::Status::Ok);
	device.answer();
	CHECK(inFlight.poll() == cl::Status::Ok && inFlight.inFlight());
	inFlight.cancel();
	CHECK(inFlight.finished() && inFlight.state() == files::ReadState::Cancelled);
	CHECK(inFlight.poll() == cl::Status::Ok && device.pending.empty());

	// A piece the queue can no longer cancel completes, and nothing follows it.
	Device sent;
	Collected sentBytes;
	files::FileReader<Device, Sink> late{sent, Sink{&sentBytes}};
	CHECK(late.start(0) == cl::Status::Ok);
	sent.answer();
	CHECK(late.poll() == cl::Status::Ok && late.inFlight());
	auto held = std::move(sent.pending.front());
	sent.pending.pop_front(); // As if already on the wire: cancel finds nothing.
	late.cancel();
	CHECK(!late.finished());
	sent.pending.push_front(std::move(held));
	sent.answer();
	CHECK(late.finished() && late.state() == files::ReadState::Cancelled &&
	      sentBytes.bytes.size() == 16);

	// Cancel between pieces ends it at once.
	Reader between{device, Sink{&collected}};
	CHECK(between.start(0) == cl::Status::Ok);
	device.answer();
	CHECK(between.due());
	between.cancel();
	CHECK(between.finished() && between.state() == files::ReadState::Cancelled &&
	      device.pending.empty());

	// The queue cancelling the request ends the read as cancelled too.
	Reader queueCancelled{device, Sink{&collected}};
	CHECK(queueCancelled.start(0) == cl::Status::Ok);
	device.fail(cl::Status::Cancelled);
	CHECK(queueCancelled.state() == files::ReadState::Cancelled);
}

void submissions()
{
	Device device;
	Collected collected;
	// A busy queue leaves the read due; the next poll submits it.
	Reader busy{device, Sink{&collected}};
	device.refuseNext = cl::Status::Busy;
	CHECK(busy.start(0) == cl::Status::Busy && busy.state() == files::ReadState::Running &&
	      busy.due());
	CHECK(busy.poll() == cl::Status::Ok && busy.inFlight());
	drive(device, busy);
	CHECK(busy.state() == files::ReadState::Complete);
	// Any other refusal ends it.
	Reader refused{device, Sink{&collected}};
	device.refuseNext = cl::Status::NotAdmitted;
	CHECK(refused.start(0) == cl::Status::NotAdmitted);
	CHECK(refused.finished() && refused.state() == files::ReadState::Failed &&
	      refused.clientStatus() == cl::Status::NotAdmitted);
}
} // namespace

int main()
{
	listing();
	complete();
	endings();
	cancelling();
	submissions();
	std::printf("Device files reader: %u checks, 0 failures\n", checks);
	return 0;
}

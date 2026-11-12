/*
 * @file Client.hpp
 * @brief The device's files over the client's one queue: list, stat, read and write in pieces.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 *
 * Every request is a raw Resource List, Stat, Read or Write submitted as
 * client::Operation::Resource on the same queue as every other request, so file
 * traffic never bypasses it. Results are parsed inside the completion and
 * handed to the caller's callback; their views borrow the queue's result
 * storage and are valid only during that callback.
 *
 * A client is anything with submitResource(request, replyBytes, callback,
 * timeoutMs): DeviceClient, DynamicClient, or an adapter declared by
 * specialising Submitter (the Workbench has one in Workbench.hpp).
 */
#ifndef TELEMETRY_INTEGRATION_FILES_CLIENT_HPP
#define TELEMETRY_INTEGRATION_FILES_CLIENT_HPP
#pragma once

#include <client/ClientResult.hpp>
#include <resource/protocol/Packets.hpp>
#include <array>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace telemetry::integration::files {
namespace rc = ::resource::protocol::client;

// The largest Resource reply: a chunk header and a full payload. A Read request
// cannot limit the size of the piece the device answers with, so a client needs
// result storage for the largest piece its device sends: this bound, or the
// device's known piece size plus the chunk header. A smaller replyBytes turns a
// larger piece into MalformedReply.
inline constexpr std::uint32_t maxReplyBytes = static_cast<std::uint32_t>(
    ::resource::protocol::wire::chunkReplyHeaderSize + ::resource::protocol::wire::maxPayloadSize);

// How a client submits a raw Resource request. Specialise it for a client
// type whose entry point differs.
template<class Client>
struct Submitter {
	template<class F>
	[[nodiscard]] static client::Submission submit(Client& owner, client::Input request,
	                                               std::uint32_t replyBytes, F&& callback,
	                                               std::uint32_t timeoutMs) noexcept
	{
		return owner.submitResource(request, replyBytes, std::forward<F>(callback), timeoutMs);
	}

	// Cancels a queued request where the client can; false where it cannot,
	// and the request then completes or times out on its own.
	static bool cancel(Client& owner, client::RequestHandle handle) noexcept
	{
		if constexpr (requires { owner.cancel(handle); })
			return owner.cancel(handle);
		else
			return false;
	}
};

// One parsed reply. `operation` is the queue's result. `parsed` is false when
// the reply bytes are not a well-formed reply of that operation; the device's
// own verdict is reply.status (a one-byte short error reads as InvalidData).
template<class Reply>
struct Result {
	client::ResultView operation{};
	bool parsed = false;
	Reply reply{};

	[[nodiscard]] bool succeeded() const noexcept
	{
		return operation.succeeded() && parsed && reply.status == ::resource::Status::Ok;
	}
};

using ListResult = Result<rc::ListReply>;
using StatResult = Result<rc::StatReply>;
using ReadResult = Result<rc::ReadReply>;
using WriteResult = Result<rc::WriteReply>;

namespace detail {
template<class Reply, class F>
struct Parsing {
	F callable;
	// A WRITE reply is checked against the payload length it answers.
	std::size_t submitted = 0;

	void operator()(const client::ResultView& operation) noexcept
	{
		Result<Reply> result{operation, false, {}};
		if (operation.status == client::Status::Ok) {
			if constexpr (std::is_same_v<Reply, rc::WriteReply>) {
				const auto parsed = rc::parseWrite(operation.payload, submitted);
				result.parsed = bool(parsed);
				result.reply = parsed.response;
			} else if constexpr (std::is_same_v<Reply, rc::ListReply>) {
				const auto parsed = rc::parseList(operation.payload);
				result.parsed = bool(parsed);
				result.reply = parsed.response;
			} else if constexpr (std::is_same_v<Reply, rc::StatReply>) {
				const auto parsed = rc::parseStat(operation.payload);
				result.parsed = bool(parsed);
				result.reply = parsed.response;
			} else {
				const auto parsed = rc::parseRead(operation.payload);
				result.parsed = bool(parsed);
				result.reply = parsed.response;
			}
		}
		callable(result);
	}
};

template<class Reply, class Client, class F>
[[nodiscard]] client::Submission submit(Client& owner, client::Input request, F&& callback,
                                        std::uint32_t timeoutMs, std::uint32_t replyBytes,
                                        std::size_t submitted = 0) noexcept
{
	static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const Result<Reply>&>,
	              "A file completion must be noexcept");
	static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>,
	              "A file completion must be constructed without exceptions");
	if (request.empty() || !replyBytes || replyBytes > maxReplyBytes)
		return {client::Status::InvalidArgument, {}};
	return Submitter<Client>::submit(owner, request, replyBytes,
	                                 Parsing<Reply, std::decay_t<F>>{std::forward<F>(callback), submitted},
	                                 timeoutMs);
}

template<class Reply, class Client, class F, std::size_t N>
[[nodiscard]] client::Submission submit(Client& owner, const std::array<std::byte, N>& request,
                                        std::size_t written, F&& callback, std::uint32_t timeoutMs,
                                        std::uint32_t replyBytes) noexcept
{
	return submit<Reply>(owner, client::Input{request.data(), written}, std::forward<F>(callback),
	                     timeoutMs, replyBytes);
}
} // namespace detail

// One page of the device's file names from cursor (0 for the first page). A
// file's index is its position in the whole list: cursor plus its place on
// the page.
template<class Client, class F>
[[nodiscard]] client::Submission list(Client& owner, ::resource::Cursor cursor, F&& callback,
                                      std::uint32_t timeoutMs = 30000,
                                      std::uint32_t replyBytes = maxReplyBytes) noexcept
{
	std::array<std::byte, ::resource::protocol::wire::listRequestSize> request{};
	const auto built = rc::makeList(request, cursor);
	return detail::submit<rc::ListReply>(owner, request, built.written, std::forward<F>(callback),
	                                     timeoutMs, replyBytes);
}

// The size and access flags of the file at index.
template<class Client, class F>
[[nodiscard]] client::Submission stat(Client& owner, ::resource::FileIndex index, F&& callback,
                                      std::uint32_t timeoutMs = 30000,
                                      std::uint32_t replyBytes = maxReplyBytes) noexcept
{
	std::array<std::byte, ::resource::protocol::wire::statRequestSize> request{};
	const auto built = rc::makeStat(request, index);
	return detail::submit<rc::StatReply>(owner, request, built.written, std::forward<F>(callback),
	                                     timeoutMs, replyBytes);
}

// One piece of the file at index, from cursor (0 for the start). The reply's
// next cursor continues it; eof ends it. Cursors are the device's own state.
template<class Client, class F>
[[nodiscard]] client::Submission
read(Client& owner, ::resource::FileIndex index, ::resource::Cursor cursor, F&& callback,
     std::uint32_t timeoutMs = 30000, std::uint32_t replyBytes = maxReplyBytes) noexcept
{
	std::array<std::byte, ::resource::protocol::wire::readRequestSize> request{};
	const auto built = rc::makeRead(request, index, cursor);
	return detail::submit<rc::ReadReply>(owner, request, built.written, std::forward<F>(callback),
	                                     timeoutMs, replyBytes);
}

// One piece written to the file at index, from cursor (0 starts a transfer),
// built in the caller's scratch (16 bytes and the piece). final marks the
// piece that ends the stream. The reply's consumed counts what the device
// took, next continues it, complete says the device finished the file. A
// WRITE is delivered again if submitted again: never resend one whose reply
// was lost unless the provider tolerates it.
template<class Client, class F>
[[nodiscard]] client::Submission
write(Client& owner, ::resource::Output scratch, ::resource::FileIndex index, ::resource::Cursor cursor,
      client::Input data, bool final, F&& callback, std::uint32_t timeoutMs = 30000,
      std::uint32_t replyBytes = ::resource::protocol::wire::writeReplySize) noexcept
{
	const auto built = rc::makeWrite(scratch, index, cursor, data, final);
	if (built.status != rc::BuildStatus::Ok)
		return {client::Status::InvalidArgument, {}};
	return detail::submit<rc::WriteReply>(owner, client::Input{scratch.data(), built.written},
	                                      std::forward<F>(callback), timeoutMs, replyBytes, data.size());
}

enum class ReadState : std::uint8_t {
	Idle,
	Running,
	Complete,
	Cancelled,
	// The queue refused or failed a request; see clientStatus().
	Failed,
	// The device answered with an error status; see deviceStatus().
	Refused,
	NotReadable,
	// A reply was not a well-formed reply of its operation.
	Malformed,
	// The sink asked to stop, for example when its disk write failed.
	SinkStopped,
	// The device kept answering without data and without moving its cursor.
	Stalled
};

// Reads one whole file: Stat, then Read piece by piece into a sink until the
// device reports its end. One request is in flight at a time.
//
// The next request is submitted by poll(), from the owner's loop, never from a
// completion callback: a Workbench refuses submissions while one of its
// completions runs. Call poll() whenever the owner loop turns, or at once after
// a piece arrived. The reader must outlive its last completion: keep it until
// finished() and no request is in flight, or until the client has drained.
//
// A file that grows while it is read ends where the device reports its end;
// the size from Stat is for progress only.
//
// Sink is callable as bool(client::Input piece) noexcept; returning false
// stops the read. The piece is borrowed for the call only.
template<class Client, class Sink>
class FileReader {
public:
	FileReader(Client& owner, Sink sink, std::uint32_t timeoutMs = 30000,
	           std::uint32_t replyBytes = maxReplyBytes) noexcept
	    : owner_(owner), sink_(std::move(sink)), timeoutMs_(timeoutMs), replyBytes_(replyBytes)
	{
		static_assert(std::is_nothrow_invocable_r_v<bool, Sink&, client::Input>,
		              "A file sink must be callable as bool(client::Input) noexcept");
	}

	FileReader(const FileReader&) = delete;
	FileReader& operator=(const FileReader&) = delete;

	// A completion still queued would call into a destroyed reader.
	~FileReader()
	{
		assert(!inFlight_ && "FileReader destroyed with a request in flight");
	}

	// Starts reading the file at index. Busy while a read is running.
	[[nodiscard]] client::Status start(::resource::FileIndex index) noexcept
	{
		if (state_ == ReadState::Running || inFlight_)
			return client::Status::Busy;
		index_ = index;
		cursor_ = 0;
		size_ = received_ = 0;
		idlePieces_ = 0;
		statted_ = false;
		due_ = true;
		cancelRequested_ = false;
		clientStatus_ = client::Status::Ok;
		deviceStatus_ = ::resource::Status::Ok;
		state_ = ReadState::Running;
		return poll();
	}

	// Submits the next request when one is due. Ok when nothing was due or the
	// request was accepted; Busy when the queue cannot take it now (poll again
	// later); any other status has ended the read as Failed.
	client::Status poll() noexcept
	{
		if (state_ != ReadState::Running || inFlight_ || !due_)
			return client::Status::Ok;
		if (cancelRequested_) {
			finish(ReadState::Cancelled);
			return client::Status::Ok;
		}
		const auto submitted =
		    statted_ ? files::read(owner_, index_, cursor_, Step{this}, timeoutMs_, replyBytes_)
		             : files::stat(owner_, index_, Step{this}, timeoutMs_, replyBytes_);
		if (submitted) {
			due_ = false;
			inFlight_ = true;
			handle_ = submitted.handle;
		} else if (submitted.status != client::Status::Busy &&
		           submitted.status != client::Status::QueueFull) {
			clientStatus_ = submitted.status;
			finish(ReadState::Failed);
		}
		return submitted.status;
	}

	// Stops the read. A request still waiting in the queue is cancelled there,
	// where the client allows it; one already sent completes or times out.
	void cancel() noexcept
	{
		if (state_ != ReadState::Running)
			return;
		cancelRequested_ = true;
		if (!inFlight_)
			finish(ReadState::Cancelled);
		else
			(void)Submitter<Client>::cancel(owner_, handle_);
	}

	[[nodiscard]] ReadState state() const noexcept
	{
		return state_;
	}

	[[nodiscard]] bool finished() const noexcept
	{
		return state_ != ReadState::Idle && state_ != ReadState::Running && !inFlight_;
	}

	[[nodiscard]] bool inFlight() const noexcept
	{
		return inFlight_;
	}

	// Whether poll() has a request to submit.
	[[nodiscard]] bool due() const noexcept
	{
		return state_ == ReadState::Running && due_ && !inFlight_;
	}

	[[nodiscard]] ::resource::FileSize size() const noexcept
	{
		return size_;
	}

	[[nodiscard]] std::uint64_t received() const noexcept
	{
		return received_;
	}

	[[nodiscard]] client::Status clientStatus() const noexcept
	{
		return clientStatus_;
	}

	[[nodiscard]] ::resource::Status deviceStatus() const noexcept
	{
		return deviceStatus_;
	}

private:
	struct Step {
		FileReader* reader;

		void operator()(const StatResult& result) noexcept
		{
			reader->stated(result);
		}

		void operator()(const ReadResult& result) noexcept
		{
			reader->piece(result);
		}
	};

	// Shared checks of one completion; false has ended the read.
	template<class Reply>
	bool accepted(const Result<Reply>& result) noexcept
	{
		inFlight_ = false;
		if (state_ != ReadState::Running)
			return false;
		if (result.operation.status != client::Status::Ok) {
			clientStatus_ = result.operation.status;
			finish(result.operation.status == client::Status::Cancelled ? ReadState::Cancelled
			                                                            : ReadState::Failed);
			return false;
		}
		if (!result.parsed) {
			finish(ReadState::Malformed);
			return false;
		}
		if (result.reply.status != ::resource::Status::Ok) {
			deviceStatus_ = result.reply.status;
			finish(ReadState::Refused);
			return false;
		}
		return true;
	}

	void stated(const StatResult& result) noexcept
	{
		if (!accepted(result))
			return;
		if (!::resource::has(result.reply.flags, ::resource::FileFlag::Readable)) {
			finish(ReadState::NotReadable);
			return;
		}
		size_ = result.reply.size;
		statted_ = true;
		next();
	}

	void piece(const ReadResult& result) noexcept
	{
		if (!accepted(result))
			return;
		const auto data = result.reply.data;
		// Reading again from an unmoved cursor would repeat the same piece
		// forever: a device that does not move it, and has not ended, is broken.
		if (!result.reply.eof && result.reply.next == cursor_) {
			finish(data.empty() ? ReadState::Stalled : ReadState::Malformed);
			return;
		}
		if (!data.empty() && !sink_(data)) {
			finish(ReadState::SinkStopped);
			return;
		}
		received_ += data.size();
		cursor_ = result.reply.next;
		if (result.reply.eof) {
			finish(ReadState::Complete);
			return;
		}
		// The same bound discovery uses for pieces that carry nothing.
		if (data.empty() && ++idlePieces_ > 4096) {
			finish(ReadState::Stalled);
			return;
		}
		next();
	}

	void next() noexcept
	{
		if (cancelRequested_)
			finish(ReadState::Cancelled);
		else
			due_ = true;
	}

	void finish(ReadState state) noexcept
	{
		state_ = state;
		due_ = false;
	}

	Client& owner_;
	Sink sink_;
	std::uint32_t timeoutMs_;
	std::uint32_t replyBytes_;
	client::RequestHandle handle_{};
	::resource::FileIndex index_ = 0;
	::resource::Cursor cursor_ = 0;
	::resource::FileSize size_ = 0;
	std::uint64_t received_ = 0;
	unsigned idlePieces_ = 0;
	client::Status clientStatus_ = client::Status::Ok;
	::resource::Status deviceStatus_ = ::resource::Status::Ok;
	ReadState state_ = ReadState::Idle;
	bool statted_ = false, due_ = false, inFlight_ = false, cancelRequested_ = false;
};

enum class WriteState : std::uint8_t {
	Idle,
	Running,
	// The device took every byte and said the file is complete.
	Complete,
	// Stopped between pieces; written() says how far it got. What the device
	// keeps of an unfinished write is its provider's (a staged one keeps the
	// old file whole).
	Cancelled,
	// The queue refused or failed a request; see clientStatus(). A timeout
	// leaves the device's file not known: the piece may or may not have been
	// taken, and it is never sent again.
	Failed,
	// The device answered with an error status; see deviceStatus().
	Refused,
	NotWritable,
	// The device's file is not the size of what was to be written. A WRITE
	// shorter or longer than the file has no meaning the protocol defines.
	SizeMismatch,
	// A reply was not a well-formed reply of its operation, or the device
	// said complete before it had every byte.
	Malformed,
	// The device kept answering without taking bytes, without moving its
	// cursor, or without completing after the final piece.
	Stalled
};

// Writes one whole file: Stat, then Write piece by piece, the last piece marked
// final, until the device says the file is complete. One request is in flight
// at a time, and, as for FileReader, the next is submitted by poll() from the
// owner's loop, never from a completion.
//
// The bytes are the caller's: borrowed from start() until finished(). They are
// sent in pieces of at most chunkBytes (at most MaxChunk), whose request is
// built in the writer's own buffer. A piece the device took only partly is
// continued from its returned cursor, still final if it was. The file must be
// writable and of the bytes' size (unless sizeChangeAllowed: a provider that
// defines replacing the whole file, such as the identity file).
//
// Nothing is ever resent on its own: WRITE has no deduplication, so a lost
// reply ends the write as Failed and the device's file is then not known.
// A provider is expected to finish the file within the request that ends it,
// as StagedFile does; one that answers "not complete yet" is asked again with
// an empty final piece at most 16 times, with no delay between, and then the
// write ends as Stalled. Stat and Write replies are reserved at their exact
// sizes (6 and 14 bytes).
template<class Client, std::size_t MaxChunk = 1024>
class FileWriter {
	static_assert(MaxChunk > 0 && MaxChunk <= ::resource::protocol::wire::maxPayloadSize,
	              "A WRITE piece holds 1 to 65535 bytes");

public:
	explicit FileWriter(Client& owner, std::uint32_t timeoutMs = 30000) noexcept
	    : owner_(owner), timeoutMs_(timeoutMs)
	{}

	FileWriter(const FileWriter&) = delete;
	FileWriter& operator=(const FileWriter&) = delete;

	// A completion still queued would call into a destroyed writer.
	~FileWriter()
	{
		assert(!inFlight_ && "FileWriter destroyed with a request in flight");
	}

	// Starts writing bytes as the whole file at index. Busy while one is running.
	[[nodiscard]] client::Status start(::resource::FileIndex index, client::Input bytes,
	                                   std::size_t chunkBytes = MaxChunk,
	                                   bool sizeChangeAllowed = false) noexcept
	{
		if (state_ == WriteState::Running || inFlight_)
			return client::Status::Busy;
		if (chunkBytes == 0)
			return client::Status::InvalidArgument;
		index_ = index;
		bytes_ = bytes;
		chunk_ = chunkBytes < MaxChunk ? chunkBytes : MaxChunk;
		sizeChange_ = sizeChangeAllowed;
		cursor_ = 0;
		sent_ = 0;
		submitted_ = 0;
		idle_ = 0;
		size_ = 0;
		finalSent_ = false;
		statted_ = false;
		due_ = true;
		cancelRequested_ = false;
		clientStatus_ = client::Status::Ok;
		deviceStatus_ = ::resource::Status::Ok;
		state_ = WriteState::Running;
		return poll();
	}

	// Submits the next request when one is due; as FileReader::poll().
	client::Status poll() noexcept
	{
		if (state_ != WriteState::Running || inFlight_ || !due_)
			return client::Status::Ok;
		if (cancelRequested_) {
			finish(WriteState::Cancelled);
			return client::Status::Ok;
		}
		client::Submission submitted;
		if (!statted_) {
			submitted = files::stat(owner_, index_, Step{this}, timeoutMs_,
			                        ::resource::protocol::wire::statReplySize);
		} else {
			const auto remaining = bytes_.size() - static_cast<std::size_t>(sent_);
			const auto count = remaining < chunk_ ? remaining : chunk_;
			const bool final = count == remaining;
			submitted = files::write(owner_, ::resource::Output{buffer_.data(), buffer_.size()}, index_,
			                         cursor_, bytes_.subspan(static_cast<std::size_t>(sent_), count),
			                         final, Step{this}, timeoutMs_, ::resource::protocol::wire::writeReplySize);
			if (submitted) {
				submitted_ = count;
				finalSent_ = final;
			}
		}
		if (submitted) {
			due_ = false;
			inFlight_ = true;
			handle_ = submitted.handle;
		} else if (submitted.status != client::Status::Busy &&
		           submitted.status != client::Status::QueueFull) {
			clientStatus_ = submitted.status;
			finish(WriteState::Failed);
		}
		return submitted.status;
	}

	// Stops before the next piece. A request still waiting in the queue is
	// cancelled there where the client allows it; one already sent completes
	// or times out, and what it wrote stays written.
	void cancel() noexcept
	{
		if (state_ != WriteState::Running)
			return;
		cancelRequested_ = true;
		if (!inFlight_)
			finish(WriteState::Cancelled);
		else
			(void)Submitter<Client>::cancel(owner_, handle_);
	}

	[[nodiscard]] WriteState state() const noexcept
	{
		return state_;
	}

	[[nodiscard]] bool finished() const noexcept
	{
		return state_ != WriteState::Idle && state_ != WriteState::Running && !inFlight_;
	}

	[[nodiscard]] bool inFlight() const noexcept
	{
		return inFlight_;
	}

	[[nodiscard]] bool due() const noexcept
	{
		return state_ == WriteState::Running && due_ && !inFlight_;
	}

	// The device's file size from Stat.
	[[nodiscard]] ::resource::FileSize size() const noexcept
	{
		return size_;
	}

	// Bytes the device has taken, of total().
	[[nodiscard]] std::uint64_t written() const noexcept
	{
		return sent_;
	}

	[[nodiscard]] std::uint64_t total() const noexcept
	{
		return bytes_.size();
	}

	[[nodiscard]] client::Status clientStatus() const noexcept
	{
		return clientStatus_;
	}

	[[nodiscard]] ::resource::Status deviceStatus() const noexcept
	{
		return deviceStatus_;
	}

private:
	struct Step {
		FileWriter* writer;

		void operator()(const StatResult& result) noexcept
		{
			writer->stated(result);
		}

		void operator()(const WriteResult& result) noexcept
		{
			writer->piece(result);
		}
	};

	template<class Reply>
	bool accepted(const Result<Reply>& result) noexcept
	{
		inFlight_ = false;
		if (state_ != WriteState::Running)
			return false;
		if (result.operation.status != client::Status::Ok) {
			clientStatus_ = result.operation.status;
			finish(result.operation.status == client::Status::Cancelled ? WriteState::Cancelled
			                                                            : WriteState::Failed);
			return false;
		}
		if (!result.parsed) {
			finish(WriteState::Malformed);
			return false;
		}
		if (result.reply.status != ::resource::Status::Ok) {
			deviceStatus_ = result.reply.status;
			finish(WriteState::Refused);
			return false;
		}
		return true;
	}

	void stated(const StatResult& result) noexcept
	{
		if (!accepted(result))
			return;
		if (!::resource::has(result.reply.flags, ::resource::FileFlag::Writable)) {
			finish(WriteState::NotWritable);
			return;
		}
		size_ = result.reply.size;
		if (!sizeChange_ && size_ != bytes_.size()) {
			finish(WriteState::SizeMismatch);
			return;
		}
		statted_ = true;
		next();
	}

	void piece(const WriteResult& result) noexcept
	{
		if (!accepted(result))
			return;
		const auto& reply = result.reply;
		sent_ += reply.consumed;
		const bool moved = reply.consumed > 0 || reply.next != cursor_;
		cursor_ = reply.next;
		const bool all = sent_ == bytes_.size();
		if (reply.complete) {
			finish(all ? WriteState::Complete : WriteState::Malformed);
			return;
		}
		// Everything sent and final, not complete: an empty final asks the
		// device to finish, a bounded number of times.
		if (all && finalSent_ && reply.consumed == submitted_) {
			if (++idle_ > maxIdle) {
				finish(WriteState::Stalled);
				return;
			}
			next();
			return;
		}
		if (!moved) {
			if (++idle_ > maxIdle) {
				finish(WriteState::Stalled);
				return;
			}
		} else {
			idle_ = 0;
		}
		next();
	}

	void next() noexcept
	{
		if (cancelRequested_)
			finish(WriteState::Cancelled);
		else
			due_ = true;
	}

	void finish(WriteState state) noexcept
	{
		state_ = state;
		due_ = false;
	}

	static constexpr unsigned maxIdle = 16;

	Client& owner_;
	std::uint32_t timeoutMs_;
	std::array<std::byte, ::resource::protocol::wire::writeRequestHeaderSize + MaxChunk> buffer_{};
	client::RequestHandle handle_{};
	client::Input bytes_{};
	::resource::FileIndex index_ = 0;
	::resource::Cursor cursor_ = 0;
	::resource::FileSize size_ = 0;
	std::uint64_t sent_ = 0;
	std::size_t chunk_ = MaxChunk;
	std::size_t submitted_ = 0;
	unsigned idle_ = 0;
	client::Status clientStatus_ = client::Status::Ok;
	::resource::Status deviceStatus_ = ::resource::Status::Ok;
	WriteState state_ = WriteState::Idle;
	bool sizeChange_ = false, finalSent_ = false, statted_ = false, due_ = false, inFlight_ = false,
	     cancelRequested_ = false;
};
} // namespace telemetry::integration::files
#endif

/*
 * @file Client.hpp
 * @brief The device's files over the client's one queue: list, stat, read in pieces.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 *
 * Every request is a raw Resource List, Stat or Read submitted as
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
#include <cstdint>
#include <type_traits>
#include <utility>

namespace telemetry::integration::files {
namespace rc = ::resource::protocol::client;

// The largest Resource reply: a chunk header and a full payload.
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

namespace detail {
template<class Reply, class F>
struct Parsing {
	F callable;

	void operator()(const client::ResultView& operation) noexcept
	{
		Result<Reply> result{operation, false, {}};
		if (operation.status == client::Status::Ok) {
			if constexpr (std::is_same_v<Reply, rc::ListReply>) {
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

template<class Reply, class Client, class F, std::size_t N>
[[nodiscard]] client::Submission submit(Client& owner, const std::array<std::byte, N>& request,
                                        std::size_t written, F&& callback, std::uint32_t timeoutMs,
                                        std::uint32_t replyBytes) noexcept
{
	static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const Result<Reply>&>,
	              "A file completion must be noexcept");
	static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>,
	              "A file completion must be constructed without exceptions");
	if (!written || !replyBytes || replyBytes > maxReplyBytes)
		return {client::Status::InvalidArgument, {}};
	return Submitter<Client>::submit(owner, client::Input{request.data(), written}, replyBytes,
	                                 Parsing<Reply, std::decay_t<F>>{std::forward<F>(callback)},
	                                 timeoutMs);
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
		} else if (submitted.status != client::Status::Busy &&
		           submitted.status != client::Status::QueueFull) {
			clientStatus_ = submitted.status;
			finish(ReadState::Failed);
		}
		return submitted.status;
	}

	// Stops after the request in flight, if any; that one completes or times out.
	void cancel() noexcept
	{
		if (state_ != ReadState::Running)
			return;
		cancelRequested_ = true;
		if (!inFlight_)
			finish(ReadState::Cancelled);
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
		if (!data.empty() && !sink_(data)) {
			finish(ReadState::SinkStopped);
			return;
		}
		received_ += data.size();
		const bool moved = result.reply.next != cursor_;
		cursor_ = result.reply.next;
		if (result.reply.eof) {
			finish(ReadState::Complete);
			return;
		}
		// The same bound discovery uses for pieces that carry nothing.
		if (data.empty() && (!moved || ++idlePieces_ > 4096)) {
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
} // namespace telemetry::integration::files
#endif

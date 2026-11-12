/*
 * @file Framing.hpp
 * @brief Caller-buffered native Resource/TEQ1 stream boundaries without extra wire bytes.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 *
 * One owner feeds arbitrary byte chunks and retains each unconsumed suffix.
 * A ready frame borrows stable storage until release(). Framing failures latch
 * until reset() at a genuinely new connection boundary. Resource replies need
 * the one outstanding operation; timeout/EOF never completes a short error.
 */
#ifndef TELEMETRY_INTEGRATION_TCP_FRAMING_HPP
#define TELEMETRY_INTEGRATION_TCP_FRAMING_HPP
#pragma once

#include <resource/Types.hpp>
#include <resource/protocol/Wire.hpp>
#include <wire/EncodedWire.hpp>
#include <algorithm>
#include <cstring>

namespace telemetry::integration::tcp {
using Input = resource::Input;
using Output = resource::Output;

enum class Mode : std::uint8_t {
	Request,
	EncodedReply,
	ResourceReply
};
enum class Status : std::uint8_t {
	NeedMore,
	Ready,
	Blocked,
	Invalid,
	TooLarge,
	Closed,
	Reentrant
};

struct FeedResult {
	Status status = Status::NeedMore;
	std::size_t consumed = 0;
};

// Public methods:
// - FrameReader(): Borrow bounded stable input storage.
// - feed(): Consume at most one frame from disjoint input and report its exact prefix.
//   Input overlapping storage is refused without consuming bytes or changing state.
// - frame()/ready()/partial()/empty()/failed(): Inspect framing state.
// - release(): Retire a ready frame without changing its bytes.
// - expect(): Change reply interpretation only at an empty boundary.
// - close(): Latch closure without releasing external transport storage.
// - reset(): Begin a new, externally established connection boundary.
class FrameReader {
public:
	explicit FrameReader(Output storage, Mode mode = Mode::Request,
	                     resource::protocol::Op expected = resource::protocol::Op::Read) noexcept
	    : storage_(storage), mode_(mode), operation_(expected)
	{}

	FrameReader(const FrameReader&) = delete;
	FrameReader& operator=(const FrameReader&) = delete;
	FrameReader(FrameReader&&) = delete;
	FrameReader& operator=(FrameReader&&) = delete;

	[[nodiscard]] FeedResult feed(Input bytes) noexcept
	{
		if (failed())
			return {status_, 0};
		if (ready())
			return {Status::Ready, 0};
		if (!bytes.empty() && !storage_.empty()) {
			const auto source = reinterpret_cast<std::uintptr_t>(bytes.data());
			const auto destination = reinterpret_cast<std::uintptr_t>(storage_.data());
			if (source <= destination ? destination - source < bytes.size()
			                          : source - destination < storage_.size())
				return {Status::Invalid, 0};
		}
		std::size_t consumed = 0;
		while (!bytes.empty()) {
			if (used_ == 0) {
				if (storage_.empty())
					return fail(Status::TooLarge, consumed);
				storage_[used_++] = bytes.front();
				bytes = bytes.subspan(1);
				++consumed;
				if (!prefix())
					return {status_, consumed};
			}
			const auto target = total_ != 0 ? total_ : header_;
			const auto count = std::min(target - used_, bytes.size());
			if (count != 0)
				std::memcpy(storage_.data() + used_, bytes.data(), count);
			used_ += count;
			bytes = bytes.subspan(count);
			consumed += count;
			if (used_ < target)
				break;
			if (total_ == 0 && !header())
				return {status_, consumed};
			if (used_ == total_) {
				status_ = Status::Ready;
				return {status_, consumed};
			}
		}
		return {Status::NeedMore, consumed};
	}

	[[nodiscard]] Input frame() const noexcept
	{
		return ready() ? Input{storage_}.first(used_) : Input{};
	}

	[[nodiscard]] bool ready() const noexcept
	{
		return status_ == Status::Ready;
	}

	[[nodiscard]] bool partial() const noexcept
	{
		return used_ != 0 && !ready();
	}

	[[nodiscard]] bool empty() const noexcept
	{
		return used_ == 0;
	}

	[[nodiscard]] bool failed() const noexcept
	{
		return status_ == Status::Invalid || status_ == Status::TooLarge ||
		       status_ == Status::Closed;
	}

	void release() noexcept
	{
		if (ready()) {
			used_ = header_ = total_ = 0;
			status_ = Status::NeedMore;
		}
	}

	[[nodiscard]] bool
	expect(Mode mode, resource::protocol::Op operation = resource::protocol::Op::Read) noexcept
	{
		if (!empty() || failed())
			return false;
		mode_ = mode;
		operation_ = operation;
		return true;
	}

	void close() noexcept
	{
		status_ = Status::Closed;
	}

	void reset(Mode mode = Mode::Request,
	           resource::protocol::Op operation = resource::protocol::Op::Read) noexcept
	{
		used_ = header_ = total_ = 0;
		status_ = Status::NeedMore;
		mode_ = mode;
		operation_ = operation;
	}

private:
	[[nodiscard]] FeedResult fail(Status status, std::size_t consumed) noexcept
	{
		status_ = status;
		return {status_, consumed};
	}

	[[nodiscard]] std::uint32_t load(std::size_t offset, unsigned width) const noexcept
	{
		std::uint32_t result = 0;
		for (unsigned i = 0; i < width; ++i)
			result |= std::uint32_t(std::to_integer<unsigned>(storage_[offset + i])) << (i * 8);
		return result;
	}

	[[nodiscard]] bool prefix() noexcept
	{
		using resource::protocol::Op;
		const auto first = std::to_integer<unsigned>(storage_[0]);
		encoded_ = mode_ == Mode::EncodedReply || (mode_ == Mode::Request && first == 0x54);
		if (encoded_) {
			if (first != 0x54) {
				status_ = Status::Invalid;
				return false;
			}
			header_ = protocol::encoded::headerBytes;
		} else if (mode_ == Mode::ResourceReply) {
			if (first > static_cast<unsigned>(resource::Status::InternalError)) {
				status_ = Status::Invalid;
				return false;
			}
			switch (operation_) {
				case Op::List:
				case Op::Read:
					header_ = resource::protocol::wire::chunkReplyHeaderSize;
					break;
				case Op::Stat:
					header_ = resource::protocol::wire::statReplySize;
					break;
				case Op::Write:
					header_ = resource::protocol::wire::writeReplySize;
					break;
				case Op::Save:
					header_ = resource::protocol::wire::saveReplySize;
					break;
				default:
					status_ = Status::Invalid;
					return false;
			}
		} else {
			operation_ = static_cast<Op>(first);
			switch (operation_) {
				case Op::List:
					header_ = resource::protocol::wire::listRequestSize;
					break;
				case Op::Stat:
					header_ = resource::protocol::wire::statRequestSize;
					break;
				case Op::Read:
					header_ = resource::protocol::wire::readRequestSize;
					break;
				case Op::Write:
					header_ = resource::protocol::wire::writeRequestHeaderSize;
					break;
				case Op::Save:
					header_ = resource::protocol::wire::saveRequestSize;
					break;
				default:
					status_ = Status::Invalid;
					return false;
			}
		}
		if (header_ > storage_.size()) {
			status_ = Status::TooLarge;
			return false;
		}
		return true;
	}

	[[nodiscard]] bool header() noexcept
	{
		std::size_t payload = 0;
		if (encoded_)
			payload = load(20, 4);
		else if (mode_ == Mode::Request && operation_ == resource::protocol::Op::Write) {
			if (std::to_integer<unsigned>(storage_[13]) > 1) {
				status_ = Status::Invalid;
				return false;
			}
			payload = load(14, 2);
		} else if (mode_ == Mode::ResourceReply && (operation_ == resource::protocol::Op::List ||
		                                            operation_ == resource::protocol::Op::Read)) {
			if (std::to_integer<unsigned>(storage_[9]) > 1) {
				status_ = Status::Invalid;
				return false;
			}
			payload = load(10, 2);
		}
		if (payload > storage_.size() - header_) {
			status_ = Status::TooLarge;
			return false;
		}
		total_ = header_ + payload;
		if (encoded_) {
			// parse() inspects only this initialized header; it does not read payload.
			const auto checked = protocol::encoded::parse(Input{storage_}.first(total_));
			if (!checked || checked.header.reply != (mode_ == Mode::EncodedReply)) {
				status_ = Status::Invalid;
				return false;
			}
		}
		return true;
	}

	Output storage_;
	Mode mode_;
	resource::protocol::Op operation_;
	std::size_t used_ = 0, header_ = 0, total_ = 0;
	Status status_ = Status::NeedMore;
	bool encoded_ = false;
};
} // namespace telemetry::integration::tcp
#endif // TELEMETRY_INTEGRATION_TCP_FRAMING_HPP

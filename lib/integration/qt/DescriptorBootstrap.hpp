/*
 * @file DescriptorBootstrap.hpp
 * @brief Sequential Resource discovery shared by the application's transports.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_QT_DESCRIPTOR_BOOTSTRAP_HPP
#define TELEMETRY_INTEGRATION_QT_DESCRIPTOR_BOOTSTRAP_HPP
#pragma once

#include <resource/protocol/Packets.hpp>
#include <telemetry/type/WireTypes.hpp>
#include <wire/ReadManyWire.hpp>
#include <wire/BatchWire.hpp>
#include <QtCore/QByteArray>
#include <QtCore/QStringList>
#include <limits>
#include <optional>
#include <span>

namespace telemetry::integration::qt {
class DescriptorBootstrap {
public:
	// Why discovery refused a device.
	enum class Refusal : std::uint8_t {
		None,
		// A reply that does not follow the Resource protocol, or a broken list.
		Protocol,
		// The list is complete and has no /telemetry/descriptor.bin.
		NoDescriptor,
		// The device refused to stat or read the descriptor, or it is not readable.
		DescriptorUnreadable,
		// The descriptor's size is outside what a client accepts.
		DescriptorSize,
		// Reading the descriptor gave more or fewer bytes than its Stat size.
		DescriptorLength,
		// A ReadMany, Batch or session file is missing, malformed or refused.
		Capability
	};

	static constexpr qsizetype maxPaths = 4096;

	void reset(bool requireReadMany = false)
	{
		*this = DescriptorBootstrap{};
		requireReadMany_ = requireReadMany;
	}

	// Optional named TCP profile. Both control/capability files must be present
	// together; their absence retains the Descriptor-only legacy contract.
	void resetOptionalReadMany(std::uint32_t requestedSession)
	{
		reset();
		optionalReadMany_ = true;
		requestedSession_ = requestedSession;
	}

	void resetOptionalEncoded(std::uint32_t requestedSession)
	{
		resetOptionalReadMany(requestedSession);
		optionalBatch_ = true;
	}

	resource::protocol::Op operation() const noexcept
	{
		return operation_;
	}

	bool complete() const noexcept
	{
		return complete_;
	}

	const QByteArray& descriptor() const noexcept
	{
		return descriptor_;
	}

	// Owned bytes; complete() also requires their canonical TRM1 syntax when
	// explicitly selected. The caller validates the Descriptor fingerprint.
	const QByteArray& capability() const noexcept
	{
		return capability_;
	}

	const QByteArray& batchCapability() const noexcept
	{
		return batchCapability_;
	}

	resource::protocol::client::BuildResult request(std::span<std::byte> output) const noexcept
	{
		namespace rc = resource::protocol::client;
		using Op = resource::protocol::Op;
		if (complete_)
			return {rc::BuildStatus::BufferTooSmall, 0};
		if (operation_ == Op::List)
			return rc::makeList(output, cursor_);
		if (writingSession_) {
			std::array<std::byte, 4> bytes{};
			for (unsigned i = 0; i < 4; ++i)
				bytes[i] = std::byte((requestedSession_ >> (8 * i)) & 0xffu);
			return sessionIndex_ ? rc::makeWrite(output, *sessionIndex_, 0, bytes, true)
			                     : rc::BuildResult{rc::BuildStatus::BufferTooSmall, 0};
		}
		const auto index = checkingSession_     ? sessionIndex_
		                   : readingCapability_ ? (readingBatch_ ? batchIndex_ : capabilityIndex_)
		                                        : index_;
		if (!index)
			return {rc::BuildStatus::BufferTooSmall, 0};
		return operation_ == Op::Stat ? rc::makeStat(output, *index)
		                              : rc::makeRead(output, *index, cursor_);
	}

	bool accept(std::span<const std::byte> bytes)
	{
		namespace rc = resource::protocol::client;
		using Op = resource::protocol::Op;
		if (complete_)
			return refuse(Refusal::Protocol);
		if (operation_ == Op::List) {
			const auto reply = rc::parseList(bytes);
			if (!reply || reply.shortError || reply.response.status != resource::Status::Ok ||
			    ++pages_ > 4096 ||
			    reply.response.paths.size() >
			        std::numeric_limits<resource::Cursor>::max() - cursor_ ||
			    reply.response.next != cursor_ + reply.response.paths.size() ||
			    reply.response.next > std::numeric_limits<resource::FileIndex>::max())
				return refuse(Refusal::Protocol);
			auto index = cursor_;
			for (const auto path : reply.response.paths) {
				if (paths_.size() < maxPaths)
					paths_.append(
					    QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
				// One path listed twice is a broken list, not a missing file.
				if (path == "/telemetry/descriptor.bin") {
					if (index_)
						return refuse(Refusal::Protocol);
					index_ = static_cast<resource::FileIndex>(index);
				}
				if ((requireReadMany_ || optionalReadMany_) && path == "/telemetry/read-many.bin") {
					if (capabilityIndex_)
						return refuse(Refusal::Protocol);
					capabilityIndex_ = static_cast<resource::FileIndex>(index);
				}
				if (optionalReadMany_ && path == "/telemetry/session.bin") {
					if (sessionIndex_)
						return refuse(Refusal::Protocol);
					sessionIndex_ = static_cast<resource::FileIndex>(index);
				}
				if (optionalBatch_ && path == "/telemetry/batch.bin") {
					if (batchIndex_)
						return refuse(Refusal::Protocol);
					batchIndex_ = static_cast<resource::FileIndex>(index);
				}
				++index;
			}
			if (!reply.response.eof && reply.response.next == cursor_)
				return refuse(Refusal::Protocol);
			cursor_ = reply.response.next;
			if (reply.response.eof) {
				listed_ = true;
				if (!index_)
					return refuse(Refusal::NoDescriptor);
				if ((requireReadMany_ && !capabilityIndex_) ||
				    (optionalReadMany_ &&
				     (!requestedSession_ ||
				      bool(capabilityIndex_ || batchIndex_) != bool(sessionIndex_))))
					return refuse(Refusal::Capability);
				operation_ = Op::Stat;
			}
		} else if (operation_ == Op::Stat) {
			const auto reply = rc::parseStat(bytes);
			if (!reply)
				return refuse(Refusal::Protocol);
			if (checkingSession_) {
				if (reply.shortError || reply.response.status != resource::Status::Ok ||
				    reply.response.size != 4 ||
				    !resource::has(reply.response.flags, resource::FileFlag::Writable))
					return refuse(Refusal::Capability);
				checkingSession_ = false;
				writingSession_ = true;
				operation_ = Op::Write;
				return true;
			}
			const auto unusable =
			    readingCapability_ ? Refusal::Capability : Refusal::DescriptorUnreadable;
			if (reply.shortError || reply.response.status != resource::Status::Ok ||
			    !resource::has(reply.response.flags, resource::FileFlag::Readable))
				return refuse(unusable);
			if (readingCapability_
			        ? reply.response.size !=
			              telemetry::protocol::encoded::read_many::capabilityBytes
			        : (reply.response.size < 64 ||
			           reply.response.size > telemetry::Limits::maxDescriptorBytes)) {
				if (!readingCapability_)
					descriptorSize_ = reply.response.size;
				return refuse(readingCapability_ ? Refusal::Capability : Refusal::DescriptorSize);
			}
			size_ = reply.response.size;
			if (!readingCapability_)
				descriptorTotal_ = size_;
			auto& destination =
			    readingCapability_ ? (readingBatch_ ? batchCapability_ : capability_) : descriptor_;
			destination.reserve(static_cast<qsizetype>(size_));
			cursor_ = 0;
			cursorOnlyReads_ = 0;
			operation_ = Op::Read;
		} else if (operation_ == Op::Read) {
			const auto reply = rc::parseRead(bytes);
			if (!reply)
				return refuse(Refusal::Protocol);
			if (reply.shortError || reply.response.status != resource::Status::Ok)
				return refuse(readingCapability_ ? Refusal::Capability
				                                 : Refusal::DescriptorUnreadable);
			auto& destination =
			    readingCapability_ ? (readingBatch_ ? batchCapability_ : capability_) : descriptor_;
			const auto received = static_cast<resource::FileSize>(destination.size());
			const auto count = reply.response.data.size();
			const auto next = reply.response.next;
			// READ cursors are opaque provider state, independent of file size.
			// Bound storage by accumulated bytes and cursor-only progress by a
			// separate budget; neither arriving bytes nor cursor changes reset
			// the transport's absolute discovery deadline.
			// A device that answers without moving on is broken; one whose file
			// is longer or shorter than it listed has a broken file, and the
			// file itself is what a user downloads to see why.
			if (!reply.response.eof && count == 0 && (next == cursor_ || ++cursorOnlyReads_ > 4096))
				return refuse(Refusal::Protocol);
			if (received > size_ || count > size_ - received ||
			    (reply.response.eof && count != size_ - received))
				return refuse(readingCapability_ ? Refusal::Capability : Refusal::DescriptorLength);
			if (count != 0)
				destination.append(reinterpret_cast<const char*>(reply.response.data.data()),
				                   static_cast<qsizetype>(count));
			cursor_ = next;
			if (reply.response.eof) {
				if (!readingCapability_ && optionalReadMany_ && (capabilityIndex_ || batchIndex_)) {
					checkingSession_ = true;
					operation_ = Op::Stat;
					cursor_ = 0;
					size_ = 0;
				} else if (!readingCapability_ && requireReadMany_) {
					readingCapability_ = true;
					operation_ = Op::Stat;
					cursor_ = 0;
					size_ = 0;
				} else {
					if (readingCapability_) {
						const auto& bytes = readingBatch_ ? batchCapability_ : capability_;
						const std::span<const std::byte> input{
						    reinterpret_cast<const std::byte*>(bytes.constData()),
						    static_cast<std::size_t>(bytes.size())};
						if (readingBatch_) {
							const auto parsed =
							    telemetry::protocol::encoded::batch::parseCapability(input);
							if (!parsed || parsed.value.session != requestedSession_)
								return refuse(Refusal::Capability);
						} else {
							const auto parsed =
							    telemetry::protocol::encoded::read_many::parseCapability(input);
							if (!parsed ||
							    (optionalReadMany_ && parsed.value.session != requestedSession_))
								return refuse(Refusal::Capability);
						}
						if (!readingBatch_ && batchIndex_) {
							readingBatch_ = true;
							operation_ = Op::Stat;
							cursor_ = 0;
							size_ = 0;
							return true;
						}
					}
					complete_ = true;
				}
			}
		} else if (operation_ == Op::Write && writingSession_) {
			const auto reply = rc::parseWrite(bytes, 4);
			if (!reply)
				return refuse(Refusal::Protocol);
			if (reply.shortError || reply.response.status != resource::Status::Ok ||
			    reply.response.consumed != 4 || !reply.response.complete ||
			    reply.response.next != 4)
				return refuse(Refusal::Capability);
			writingSession_ = false;
			readingCapability_ = true;
			readingBatch_ = !capabilityIndex_;
			operation_ = Op::Stat;
			cursor_ = 0;
			size_ = 0;
		} else {
			return refuse(Refusal::Protocol);
		}
		return true;
	}

	// Why accept() refused, once it has. Only Protocol leaves the stream
	// untrustworthy; the others refuse telemetry on a device whose files were
	// listed correctly and can still be read.
	Refusal refusal() const noexcept
	{
		return refusal_;
	}

	// Every path the device listed, in its order, at most maxPaths of them.
	const QStringList& paths() const noexcept
	{
		return paths_;
	}

	// Whether the whole list was read; files can be offered only then.
	bool listed() const noexcept
	{
		return listed_;
	}

	// The descriptor's size when DescriptorSize refused it.
	resource::FileSize descriptorSize() const noexcept
	{
		return descriptorSize_;
	}

	// Bytes of the descriptor read so far, and its size from Stat; both zero
	// until its Stat is answered.
	quint64 descriptorReceived() const noexcept
	{
		return static_cast<quint64>(descriptor_.size());
	}

	quint64 descriptorTotal() const noexcept
	{
		return descriptorTotal_;
	}

private:
	bool refuse(Refusal reason) noexcept
	{
		refusal_ = reason;
		return false;
	}

	QByteArray descriptor_, capability_, batchCapability_;
	QStringList paths_;
	Refusal refusal_ = Refusal::None;
	resource::FileSize descriptorSize_ = 0;
	quint64 descriptorTotal_ = 0;
	bool listed_ = false;
	std::optional<resource::FileIndex> index_, capabilityIndex_, batchIndex_, sessionIndex_;
	resource::Cursor cursor_ = 0;
	resource::FileSize size_ = 0;
	unsigned pages_ = 0, cursorOnlyReads_ = 0;
	resource::protocol::Op operation_ = resource::protocol::Op::List;
	bool complete_ = false, requireReadMany_ = false, readingCapability_ = false;
	bool optionalReadMany_ = false, checkingSession_ = false, writingSession_ = false;
	bool optionalBatch_ = false, readingBatch_ = false;
	std::uint32_t requestedSession_ = 0;
};
} // namespace telemetry::integration::qt
#endif

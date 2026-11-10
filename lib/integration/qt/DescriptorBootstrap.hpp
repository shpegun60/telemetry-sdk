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
#include <limits>
#include <optional>
#include <span>

namespace telemetry::integration::qt {
class DescriptorBootstrap {
public:
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
			return false;
		if (operation_ == Op::List) {
			const auto reply = rc::parseList(bytes);
			if (!reply || reply.shortError || reply.response.status != resource::Status::Ok ||
			    ++pages_ > 4096 ||
			    reply.response.paths.size() >
			        std::numeric_limits<resource::Cursor>::max() - cursor_ ||
			    reply.response.next != cursor_ + reply.response.paths.size() ||
			    reply.response.next > std::numeric_limits<resource::FileIndex>::max())
				return false;
			auto index = cursor_;
			for (const auto path : reply.response.paths) {
				if (path == "/telemetry/descriptor.bin") {
					if (index_)
						return false;
					index_ = static_cast<resource::FileIndex>(index);
				}
				if ((requireReadMany_ || optionalReadMany_) && path == "/telemetry/read-many.bin") {
					if (capabilityIndex_)
						return false;
					capabilityIndex_ = static_cast<resource::FileIndex>(index);
				}
				if (optionalReadMany_ && path == "/telemetry/session.bin") {
					if (sessionIndex_)
						return false;
					sessionIndex_ = static_cast<resource::FileIndex>(index);
				}
				if (optionalBatch_ && path == "/telemetry/batch.bin") {
					if (batchIndex_)
						return false;
					batchIndex_ = static_cast<resource::FileIndex>(index);
				}
				++index;
			}
			if (!reply.response.eof && reply.response.next == cursor_)
				return false;
			cursor_ = reply.response.next;
			if (reply.response.eof) {
				if (!index_ || (requireReadMany_ && !capabilityIndex_) ||
				    (optionalReadMany_ &&
				     (!requestedSession_ ||
				      bool(capabilityIndex_ || batchIndex_) != bool(sessionIndex_))))
					return false;
				operation_ = Op::Stat;
			}
		} else if (operation_ == Op::Stat) {
			const auto reply = rc::parseStat(bytes);
			if (checkingSession_) {
				if (!reply || reply.shortError || reply.response.status != resource::Status::Ok ||
				    reply.response.size != 4 ||
				    !resource::has(reply.response.flags, resource::FileFlag::Writable))
					return false;
				checkingSession_ = false;
				writingSession_ = true;
				operation_ = Op::Write;
				return true;
			}
			if (!reply || reply.shortError || reply.response.status != resource::Status::Ok ||
			    !resource::has(reply.response.flags, resource::FileFlag::Readable))
				return false;
			if (readingCapability_ ? reply.response.size !=
			                             telemetry::protocol::encoded::read_many::capabilityBytes
			                       : (reply.response.size < 64 ||
			                          reply.response.size > telemetry::Limits::maxDescriptorBytes))
				return false;
			size_ = reply.response.size;
			auto& destination =
			    readingCapability_ ? (readingBatch_ ? batchCapability_ : capability_) : descriptor_;
			destination.reserve(static_cast<qsizetype>(size_));
			cursor_ = 0;
			cursorOnlyReads_ = 0;
			operation_ = Op::Read;
		} else if (operation_ == Op::Read) {
			const auto reply = rc::parseRead(bytes);
			if (!reply || reply.shortError || reply.response.status != resource::Status::Ok)
				return false;
			auto& destination =
			    readingCapability_ ? (readingBatch_ ? batchCapability_ : capability_) : descriptor_;
			const auto received = static_cast<resource::FileSize>(destination.size());
			const auto count = reply.response.data.size();
			const auto next = reply.response.next;
			// READ cursors are opaque provider state, independent of file size.
			// Bound storage by accumulated bytes and cursor-only progress by a
			// separate budget; neither arriving bytes nor cursor changes reset
			// the transport's absolute discovery deadline.
			if (received > size_ || count > size_ - received ||
			    (reply.response.eof && count != size_ - received) ||
			    (!reply.response.eof && count == 0 &&
			     (next == cursor_ || ++cursorOnlyReads_ > 4096)))
				return false;
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
								return false;
						} else {
							const auto parsed =
							    telemetry::protocol::encoded::read_many::parseCapability(input);
							if (!parsed ||
							    (optionalReadMany_ && parsed.value.session != requestedSession_))
								return false;
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
			if (!reply || reply.shortError || reply.response.status != resource::Status::Ok ||
			    reply.response.consumed != 4 || !reply.response.complete ||
			    reply.response.next != 4)
				return false;
			writingSession_ = false;
			readingCapability_ = true;
			readingBatch_ = !capabilityIndex_;
			operation_ = Op::Stat;
			cursor_ = 0;
			size_ = 0;
		} else {
			return false;
		}
		return true;
	}

private:
	QByteArray descriptor_, capability_, batchCapability_;
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

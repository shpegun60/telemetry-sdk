/*
 * @file Server.hpp
 * @brief One-connection native TCP adapter to existing Resource and Encoded dispatchers.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 *
 * One application owner serializes ingress, model/workspace access and closure.
 * The adapter creates no socket, task, heap allocation or execution queue.
 * Borrowed storage remains alive through all actual transport TX/RX completion.
 */
#ifndef TELEMETRY_INTEGRATION_TCP_SERVER_HPP
#define TELEMETRY_INTEGRATION_TCP_SERVER_HPP
#pragma once

#include <integration/tcp/Framing.hpp>
#include <integration/tcp/ReadManyProfile.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <resource/protocol/Protocol.hpp>

namespace telemetry::integration::tcp {
// Public methods:
// - Server(): Borrow one stable Model, Workspace, file table and disjoint RX/TX buffers.
// - receive(): Assemble one request and dispatch only when a complete reply fits.
// - reply()/releaseReply(): Borrow and retire the single pending reply after transport handoff.
// - close()/closed(): Revoke ingress without claiming physical buffer release.
// - session()/lastRequestId(): Inspect connection-bound Encoded replay guards.
class Server {
public:
	Server(ModelView model, Workspace& workspace, resource::FileSystemView files, Output rx,
	       Output tx) noexcept
	    : model_(model), workspace_(workspace), files_(files), reader_(rx), tx_(tx)
	{
		if (buffersOverlap(rx, tx) || buffersOverlap(rx, workspace.storage()) ||
		    buffersOverlap(tx, workspace.storage()) || rx.empty() || tx.empty())
			close();
	}

	// Explicit optional profile; its Resource providers bind a nonzero session
	// once before Encoded traffic. No Resource callback reenters this Server.
	Server(ModelView model, Workspace& workspace, ReadManyProfile& profile, Output rx,
	       Output tx) noexcept
	    : Server(model, workspace, profile.files(), rx, tx)
	{
		readMany_ = &profile;
		readManyDispatch_ = +[](ModelView model, Workspace& workspace, std::uint32_t session,
		                        Input request, Output response, const ReadManyProfile& value) noexcept {
			return encoded::dispatchEncoded(model, workspace, session, request, response,
			                                value.capability());
		};
		const Input control{reinterpret_cast<const std::byte*>(&profile), sizeof(profile)};
		if (buffersOverlap(rx, control) || buffersOverlap(tx, control) ||
		    buffersOverlap(workspace.storage(), control))
			close();
	}

	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;
	Server(Server&&) = delete;
	Server& operator=(Server&&) = delete;

	[[nodiscard]] FeedResult receive(Input bytes) noexcept
	{
		if (closed_)
			return {Status::Closed, 0};
		if (handling_)
			return {Status::Reentrant, 0};
		if (txBytes_ != 0)
			return {Status::Blocked, 0};
		auto result = reader_.feed(bytes);
		if (result.status != Status::Ready) {
			if (reader_.failed())
				close();
			return result;
		}
		handling_ = true;
		const auto request = reader_.frame();
		std::size_t written = 0;
		if (request[0] == std::byte{0x54}) {
			const auto parsed = protocol::encoded::parse(request);
			if (!parsed || parsed.header.reply ||
			    (readMany_ && (!readMany_->bound() ||
			                   parsed.header.session != readMany_->capability().session)) ||
			    (session_ != 0 && parsed.header.session != session_) ||
			    parsed.header.requestId <= lastRequestId_) {
				result.status = Status::Invalid;
			} else {
				if (readMany_)
					readMany_->frozen_ = true;
				if (session_ == 0)
					session_ = parsed.header.session;
				lastRequestId_ = parsed.header.requestId;
				const auto dispatched = readMany_
				    ? readManyDispatch_(model_, workspace_, session_, request, tx_, *readMany_)
				    : encoded::dispatchEncoded(model_, workspace_, session_, request, tx_);
				if (dispatched)
					written = dispatched.written;
				else
					result.status =
					    dispatched.status == encoded::EncodedDispatchStatus::BufferTooSmall
					        ? Status::TooLarge
					        : Status::Invalid;
			}
		} else {
			const auto dispatched = resource::protocol::process(files_, request, tx_);
			written = dispatched.written;
			// A short malformed-request reply has no unambiguous stream boundary.
			if (written <= 1) {
				written = 0;
				result.status = Status::Invalid;
			}
		}
		handling_ = false;
		if (closed_)
			return {Status::Closed, result.consumed};
		if (written == 0) {
			close();
			return result;
		}
		txBytes_ = written;
		reader_.release();
		return {Status::Ready, result.consumed};
	}

	[[nodiscard]] Input reply() const noexcept
	{
		return !closed_ ? Input{tx_}.first(txBytes_) : Input{};
	}

	void releaseReply() noexcept
	{
		txBytes_ = 0;
	}

	void close() noexcept
	{
		closed_ = true;
		reader_.close();
		txBytes_ = 0;
	}

	[[nodiscard]] bool closed() const noexcept
	{
		return closed_;
	}

	[[nodiscard]] std::uint32_t session() const noexcept
	{
		return session_;
	}

	[[nodiscard]] std::uint32_t lastRequestId() const noexcept
	{
		return lastRequestId_;
	}

private:
	ModelView model_;
	Workspace& workspace_;
	resource::FileSystemView files_;
	FrameReader reader_;
	Output tx_;
	std::size_t txBytes_ = 0;
	std::uint32_t session_ = 0, lastRequestId_ = 0;
	ReadManyProfile* readMany_ = nullptr;
	// Populate only in the opt-in constructor: legacy source-only users do not
	// acquire an unresolved reference to the optional compiled dispatcher.
	encoded::EncodedDispatchResult (*readManyDispatch_)(ModelView, Workspace&, std::uint32_t,
	                                                   Input, Output,
	                                                   const ReadManyProfile&) noexcept = nullptr;
	bool closed_ = false, handling_ = false;
};
} // namespace telemetry::integration::tcp
#endif // TELEMETRY_INTEGRATION_TCP_SERVER_HPP

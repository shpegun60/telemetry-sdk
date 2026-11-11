/*
 * @file Backend.hpp
 * @brief One-owner bounded Encoded v1 backend, without retries or transport I/O.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * TX and result storage are borrowed, disjoint and stable for this object's
 * lifetime. The port COPIES nextPacket() bytes before prepareHandoff(); after
 * authorization it owns that copy until TX release, including after timeout.
 * Clock reads and process(now) use one modulo-u32 millisecond domain. The owner
 * serializes every operation; user callbacks belong to the higher client layer.
 */
#ifndef TELEMETRY_INTEGRATION_ENCODED_BACKEND_HPP
#define TELEMETRY_INTEGRATION_ENCODED_BACKEND_HPP
#pragma once

#include <client/Backend.hpp>
#include <wire/EncodedWire.hpp>
#include <cstring>
#include <cstdlib>

namespace telemetry::integration::encoded {
namespace encoded_wire = ::telemetry::protocol::encoded;

class EncodedBackend final : public ::telemetry::client::Backend {
public:
	// enableResource admits raw Resource List, Stat and Read requests on this
	// queue. Only a port that can frame a Resource reply after discovery may set it.
	explicit EncodedBackend(::telemetry::client::Output txStorage,
	                        ::telemetry::client::Output resultStorage,
	                        ::telemetry::client::Clock clock, std::uint32_t timeoutMs = 30000,
	                        bool enableReadMany = false, bool enableBatch = false,
	                        bool enableResource = false) noexcept
	    : tx_(txStorage), result_(resultStorage), clock_(clock), timeoutMs_(timeoutMs),
	      readManyEnabled_(enableReadMany), batchEnabled_(enableBatch),
	      resourceEnabled_(enableResource)
	{
		if (clock_.read == nullptr || timeoutMs_ == 0 ||
		    timeoutMs_ > ::telemetry::client::maxTimeoutMs || overlaps(tx_, result_))
			std::abort();
	}

	EncodedBackend(const EncodedBackend&) = delete;
	EncodedBackend& operator=(const EncodedBackend&) = delete;
	EncodedBackend(EncodedBackend&&) = delete;
	EncodedBackend& operator=(EncodedBackend&&) = delete;

	// This is local transport support; peer capability is admitted separately.
	[[nodiscard]] bool supportsReadMany() const noexcept
	{
		return readManyEnabled_;
	}

	[[nodiscard]] bool supportsBatch() const noexcept
	{
		return batchEnabled_;
	}

	[[nodiscard]] bool supportsResource() const noexcept
	{
		return resourceEnabled_;
	}

	[[nodiscard]] ::telemetry::client::Status start(::telemetry::client::RequestView request,
	                                                std::uint32_t now) noexcept override
	{
		if (request.timeoutMs == 0 || request.timeoutMs > ::telemetry::client::maxTimeoutMs)
			return ::telemetry::client::Status::InvalidArgument;
		if (state_ != State::Idle)
			return ::telemetry::client::Status::Busy;
		if (recovery_)
			return ::telemetry::client::Status::RecoveryRequired;
		if (!admitted_)
			return ::telemetry::client::Status::NotAdmitted;
		if (request.operation == ::telemetry::client::Operation::ReadMany && !readManyEnabled_)
			return ::telemetry::client::Status::Unsupported;
		if (request.operation == ::telemetry::client::Operation::Batch && !batchEnabled_)
			return ::telemetry::client::Status::Unsupported;
		if (request.operation == ::telemetry::client::Operation::Resource && !resourceEnabled_)
			return ::telemetry::client::Status::Unsupported;
		if (nextId_ == UINT32_MAX)
			return ::telemetry::client::Status::IdsExhausted;
		if (request.payload.size() > UINT32_MAX)
			return ::telemetry::client::Status::InvalidArgument;
		if (result_.size() < request.responseBytes)
			return ::telemetry::client::Status::BufferTooSmall;
		if (request.operation == ::telemetry::client::Operation::Resource)
			return startResource(request, now);
		encoded_wire::Header header{};
		header.operation = static_cast<encoded_wire::Operation>(request.operation);
		header.session = generation_;
		header.requestId = nextId_ + 1;
		header.endpoint = request.endpointId;
		header.expectedResponseBytes = request.responseBytes;
		const auto built = encoded_wire::write(tx_, header, request.payload);
		if (!built)
			return built.status == encoded_wire::Status::BufferTooSmall
			           ? ::telemetry::client::Status::BufferTooSmall
			           : ::telemetry::client::Status::InvalidArgument;
		header.payloadBytes = static_cast<std::uint32_t>(request.payload.size());
		request_ = header;
		nextId_ = header.requestId;
		txBytes_ = built.written;
		deadline_ = now + (request.timeoutMs < timeoutMs_ ? request.timeoutMs : timeoutMs_);
		prepared_ = false;
		resource_ = false;
		operation_ = request.operation;
		completion_ = {};
		completion_.operation = request.operation;
		state_ = State::Offered;
		return ::telemetry::client::Status::Ok;
	}

	void process(std::uint32_t now) noexcept override
	{
		if (active() && ::telemetry::client::expired(now, deadline_))
			finish(::telemetry::client::Status::Timeout);
	}

	[[nodiscard]] bool completed() const noexcept override
	{
		return state_ == State::Completed;
	}

	[[nodiscard]] ::telemetry::client::ResultView result() const noexcept override
	{
		if (!completed()) {
			::telemetry::client::ResultView unavailable{};
			unavailable.status = ::telemetry::client::Status::Busy;
			return unavailable;
		}
		return completion_;
	}

	void releaseResult() noexcept override
	{
		if (!completed())
			return;
		completion_ = {};
		txBytes_ = 0;
		prepared_ = false;
		state_ = State::Idle;
	}

	void cancel(::telemetry::client::Status reason) noexcept override
	{
		if (active())
			finish(reason == ::telemetry::client::Status::Ok
			           ? ::telemetry::client::Status::Cancelled
			           : reason);
	}

	[[nodiscard]] bool ready() const noexcept override
	{
		return admitted_ && !recovery_;
	}

	[[nodiscard]] bool recoveryRequired() const noexcept override
	{
		return recovery_;
	}

	[[nodiscard]] std::uint32_t session() const noexcept override
	{
		return generation_;
	}

	[[nodiscard]] bool openSession(std::uint32_t generation) noexcept override
	{
		if (state_ != State::Idle || recovery_ || generation == 0 || generation <= generation_)
			return false;
		generation_ = generation;
		nextId_ = 0;
		admitted_ = true;
		return true;
	}

	// The owner has excluded/drained all old peer frames and closed that peer's
	// session. Confirmation clears only the recovery gate; a newer session must
	// still be explicitly admitted. It never starts or replays an invocation.
	[[nodiscard]] bool confirmSessionClosed(std::uint32_t generation) noexcept override
	{
		if (!recovery_ || generation != recoveryGeneration_ || active())
			return false;
		recovery_ = false;
		admitted_ = false;
		return true;
	}

	void close() noexcept override
	{
		if (active())
			finish(::telemetry::client::Status::SessionLost);
		admitted_ = false;
	}

	[[nodiscard]] ::telemetry::client::Packet nextPacket() noexcept override
	{
		if (state_ != State::Offered)
			return {};
		::telemetry::client::Packet packet{::telemetry::client::Input{tx_.data(), txBytes_},
		                                   generation_, request_.requestId};
		if (resource_) {
			packet.reply = ::telemetry::client::ReplyFrame::Resource;
			packet.resourceOperation = std::to_integer<std::uint8_t>(tx_[0]);
			// List, Stat and Read change nothing on the device, so a cancelled
			// one never counts as possibly executed.
			packet.mayExecute = false;
		}
		return packet;
	}

	[[nodiscard]] bool prepareHandoff(std::uint32_t generation,
	                                  std::uint32_t requestId) noexcept override
	{
		if (!matches(generation, requestId) || state_ != State::Offered)
			return false;
		process(clock_.now());
		if (state_ != State::Offered || !admitted_ || recovery_)
			return false;
		// From this point a port-owned copy may execute even if no later
		// acceptance notification or reply reaches this object.
		prepared_ = true;
		state_ = State::Prepared;
		return true;
	}

	[[nodiscard]] bool onSendAccepted(std::uint32_t generation,
	                                  std::uint32_t requestId) noexcept override
	{
		if (!matches(generation, requestId) || state_ != State::Prepared)
			return false;
		state_ = State::Sent;
		process(clock_.now());
		return true;
	}

	[[nodiscard]] bool onReply(::telemetry::client::Input bytes, std::uint32_t generation,
	                           std::uint32_t requestId) noexcept override
	{
		if (!matches(generation, requestId) || (state_ != State::Prepared && state_ != State::Sent))
			return false;
		process(clock_.now());
		if (!active())
			return false;
		if (resource_)
			return completeResource(bytes);
		const auto parsed = encoded_wire::parse(bytes);
		if (!parsed) {
			finish(::telemetry::client::Status::MalformedReply);
			return true;
		}
		const auto& header = parsed.header;
		if (!header.reply || header.session != request_.session ||
		    header.requestId != request_.requestId || header.operation != request_.operation ||
		    header.endpoint != request_.endpoint)
			return false;
		if (header.expectedResponseBytes != request_.expectedResponseBytes ||
		    parsed.payload.size() > result_.size()) {
			finish(::telemetry::client::Status::MalformedReply);
			return true;
		}
		if (!parsed.payload.empty())
			std::memmove(result_.data(), parsed.payload.data(), parsed.payload.size());
		completion_.status = ::telemetry::client::Status::Ok;
		completion_.dispatch = header.dispatch;
		completion_.endpointStatus = header.endpointStatus;
		completion_.payload = ::telemetry::client::Input{result_.data(), parsed.payload.size()};
		state_ = State::Completed;
		txBytes_ = 0;
		return true;
	}

private:
	enum class State : std::uint8_t {
		Idle,
		Offered,
		Prepared,
		Sent,
		Completed
	};

	[[nodiscard]] static bool overlaps(::telemetry::client::Input first,
	                                   ::telemetry::client::Input second) noexcept
	{
		if (first.empty() || second.empty())
			return false;
		const auto a = reinterpret_cast<std::uintptr_t>(first.data());
		const auto b = reinterpret_cast<std::uintptr_t>(second.data());
		return a <= b ? b - a < first.size() : a - b < second.size();
	}

	[[nodiscard]] bool active() const noexcept
	{
		return state_ == State::Offered || state_ == State::Prepared || state_ == State::Sent;
	}

	[[nodiscard]] bool matches(std::uint32_t generation, std::uint32_t requestId) const noexcept
	{
		return admitted_ && generation == generation_ && requestId == request_.requestId;
	}

	// A Resource request travels as the device's raw Resource packet, without a
	// TEQ1 header; its identity stays local, and its reply is matched by position.
	[[nodiscard]] ::telemetry::client::Status
	startResource(::telemetry::client::RequestView request, std::uint32_t now) noexcept
	{
		if (request.payload.empty() || request.endpointId != 0)
			return ::telemetry::client::Status::InvalidArgument;
		const auto operation = std::to_integer<unsigned>(request.payload[0]);
		// List, Stat and Read only; writing files is a separate, later request.
		if (operation < 1 || operation > 3)
			return ::telemetry::client::Status::Unsupported;
		if (request.payload.size() > tx_.size())
			return ::telemetry::client::Status::BufferTooSmall;
		std::memmove(tx_.data(), request.payload.data(), request.payload.size());
		request_ = {};
		request_.session = generation_;
		request_.requestId = nextId_ + 1;
		request_.expectedResponseBytes = request.responseBytes;
		nextId_ = request_.requestId;
		txBytes_ = request.payload.size();
		deadline_ = now + (request.timeoutMs < timeoutMs_ ? request.timeoutMs : timeoutMs_);
		prepared_ = false;
		resource_ = true;
		operation_ = request.operation;
		completion_ = {};
		completion_.operation = request.operation;
		state_ = State::Offered;
		return ::telemetry::client::Status::Ok;
	}

	[[nodiscard]] bool completeResource(::telemetry::client::Input bytes) noexcept
	{
		if (bytes.size() > result_.size() || bytes.size() > request_.expectedResponseBytes) {
			finish(::telemetry::client::Status::MalformedReply);
			return true;
		}
		if (!bytes.empty())
			std::memmove(result_.data(), bytes.data(), bytes.size());
		completion_.status = ::telemetry::client::Status::Ok;
		completion_.payload = ::telemetry::client::Input{result_.data(), bytes.size()};
		state_ = State::Completed;
		txBytes_ = 0;
		return true;
	}

	void finish(::telemetry::client::Status reason) noexcept
	{
		completion_.status = prepared_ && !::telemetry::client::isReadOnly(operation_)
		                         ? ::telemetry::client::Status::OutcomeUnknown
		                         : reason;
		completion_.payload = {};
		completion_.dispatch = completion_.endpointStatus = completion_.codec = 0;
		if (prepared_) {
			recovery_ = true;
			recoveryGeneration_ = generation_;
			admitted_ = false;
		}
		state_ = State::Completed;
		txBytes_ = 0;
	}

	::telemetry::client::Output tx_;
	::telemetry::client::Output result_;
	::telemetry::client::Clock clock_;
	std::uint32_t timeoutMs_;
	encoded_wire::Header request_{};
	::telemetry::client::ResultView completion_{};
	std::size_t txBytes_ = 0;
	std::uint32_t generation_ = 0;
	std::uint32_t nextId_ = 0;
	std::uint32_t deadline_ = 0;
	std::uint32_t recoveryGeneration_ = 0;
	::telemetry::client::Operation operation_ = ::telemetry::client::Operation::FieldRead;
	State state_ = State::Idle;
	bool admitted_ = false;
	bool prepared_ = false;
	bool recovery_ = false;
	bool resource_ = false;
	bool readManyEnabled_ = false;
	bool batchEnabled_ = false;
	bool resourceEnabled_ = false;
};

} // namespace telemetry::integration::encoded
#endif // TELEMETRY_INTEGRATION_ENCODED_BACKEND_HPP

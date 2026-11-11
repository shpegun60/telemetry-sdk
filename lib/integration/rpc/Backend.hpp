/* One-mailbox backend for the portable asynchronous client. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#ifndef TELEMETRY_INTEGRATION_RPC_BACKEND_HPP
#define TELEMETRY_INTEGRATION_RPC_BACKEND_HPP
#pragma once
#include <client/rpc/RpcClient.hpp>
#include <client/Backend.hpp>
#include <resource/protocol/Wire.hpp>

namespace telemetry::integration::rpc {
namespace native = ::telemetry::client::rpc;
namespace wire = ::telemetry::protocol::rpc;

// Exclusively borrows the native engine and its stable, disjoint buffers.
// Every call uses one owner. The port copies outgoing bytes before handoff.
// A reused native client must have its previous remote session closed first;
// retained non-idle state creates a conservative recovery gate with generation 0.
class RpcBackend final : public ::telemetry::client::Backend {
public:
	explicit RpcBackend(native::RpcClient& client, ::telemetry::client::Clock clock) noexcept
	    : client_(client), clock_(clock)
	{
		if (!clock_.read)
			std::abort();
		if (client_.state() != native::RpcClientState::Idle) {
			if (client_.active())
				client_.reset();
			recovery_ = true;
		}
	}

	~RpcBackend() override
	{
		close();
	}

	RpcBackend(const RpcBackend&) = delete;
	RpcBackend& operator=(const RpcBackend&) = delete;
	RpcBackend(RpcBackend&&) = delete;
	RpcBackend& operator=(RpcBackend&&) = delete;

	[[nodiscard]] ::telemetry::client::Status start(::telemetry::client::RequestView request,
	                                                std::uint32_t now) noexcept override
	{
		if (request.operation == ::telemetry::client::Operation::ReadMany ||
		    request.operation == ::telemetry::client::Operation::Batch ||
		    request.operation == ::telemetry::client::Operation::Resource)
			return ::telemetry::client::Status::Unsupported;
		if (recovery_)
			return ::telemetry::client::Status::RecoveryRequired;
		if (!opened_)
			return ::telemetry::client::Status::NotAdmitted;
		if (active_ || terminal_)
			return ::telemetry::client::Status::Busy;
		if (request.timeoutMs == 0 || request.timeoutMs > ::telemetry::client::maxTimeoutMs)
			return ::telemetry::client::Status::InvalidArgument;
		client_.process(now);
		const auto status =
		    client_.beginEncoded(static_cast<wire::RpcOperation>(request.operation),
		                         request.endpointId, request.payload, request.responseBytes);
		if (status != native::RpcClientStatus::Ok)
			return translate(status);
		operation_ = request.operation;
		deadline_ = now + request.timeoutMs;
		active_ = true;
		finalPrepared_ = false;
		preparedRequest_ = 0;
		outcome_ = {};
		return ::telemetry::client::Status::Ok;
	}

	void process(std::uint32_t now) noexcept override
	{
		if (!active_ || recovery_)
			return;
		if (::telemetry::client::expired(now, deadline_)) {
			cancel(::telemetry::client::Status::Timeout);
			return;
		}
		client_.process(now);
		settle();
	}

	[[nodiscard]] bool completed() const noexcept override
	{
		return terminal_;
	}

	[[nodiscard]] ::telemetry::client::ResultView result() const noexcept override
	{
		return terminal_ ? outcome_
		                 : ::telemetry::client::ResultView{::telemetry::client::Status::Busy};
	}

	void releaseResult() noexcept override
	{
		if (!terminal_)
			return;
		terminal_ = false;
		outcome_ = {};
	}

	void cancel(::telemetry::client::Status reason) noexcept override
	{
		if (!active_)
			return;
		client_.reset();
		settle(reason == ::telemetry::client::Status::Ok ? ::telemetry::client::Status::Cancelled
		                                                 : reason);
	}

	[[nodiscard]] bool ready() const noexcept override
	{
		return opened_ && !recovery_;
	}

	[[nodiscard]] bool recoveryRequired() const noexcept override
	{
		return recovery_;
	}

	[[nodiscard]] std::uint32_t session() const noexcept override
	{
		return session_;
	}

	[[nodiscard]] std::uint32_t recoveryGeneration() const noexcept
	{
		return session_;
	}

	[[nodiscard]] bool openSession(std::uint32_t generation) noexcept override
	{
		if (recovery_ || generation == 0 || active_ || terminal_)
			return false;
		if (opened_) {
			if (generation == session_)
				return true;
			close();
			return false;
		}
		if (client_.startSession(generation) != native::RpcClientStatus::Ok)
			return false;
		session_ = generation;
		opened_ = true;
		return true;
	}

	// The application has excluded old frames, drained TX copies and closed the
	// remote session (pool release/drain then fresh acquire). This never admits
	// a schema, replays work or claims an old effect was undone.
	[[nodiscard]] bool confirmSessionClosed(std::uint32_t generation) noexcept override
	{
		if (!recovery_ || generation != session_ || active_ || client_.active())
			return false;
		recovery_ = false;
		return true;
	}

	void close() noexcept override
	{
		if (active_)
			cancel(::telemetry::client::Status::SessionLost);
		if (opened_)
			requireRecovery();
	}

	[[nodiscard]] ::telemetry::client::Packet nextPacket() noexcept override
	{
		process(clock_.now());
		if (!ready() || !active_)
			return {};
		const auto packet = client_.nextPacket();
		settle();
		return packet ? ::telemetry::client::Packet{packet.bytes, packet.generation,
		                                            packet.requestId, finalWrite(packet)}
		              : ::telemetry::client::Packet{};
	}

	[[nodiscard]] bool prepareHandoff(std::uint32_t generation,
	                                  std::uint32_t requestId) noexcept override
	{
		process(clock_.now());
		if (!ready() || !active_)
			return false;
		const auto packet = client_.nextPacket();
		if (!packet || packet.generation != generation || packet.requestId != requestId)
			return false;
		preparedRequest_ = requestId;
		if (finalWrite(packet))
			finalPrepared_ = true;
		return true;
	}

	[[nodiscard]] bool onSendAccepted(std::uint32_t generation,
	                                  std::uint32_t requestId) noexcept override
	{
		if (!ready() || !active_)
			return false;
		const auto packet = client_.nextPacket();
		if (!packet || packet.generation != generation || packet.requestId != requestId)
			return false;
		const bool missingGate = preparedRequest_ != requestId;
		// The transfer has happened. Preserve that evidence before reporting a
		// missing pre-handoff gate; a post-send failure cannot undo the transfer.
		if (!client_.onSendAccepted(generation, requestId))
			return false;
		// Each physical send consumes its gate, including an identical ACK-loss retry.
		preparedRequest_ = 0;
		if (finalWrite(packet))
			finalPrepared_ = true;
		if (missingGate) {
			client_.loseSession(generation);
			settle(::telemetry::client::Status::MalformedReply);
			requireRecovery();
			return false;
		}
		process(clock_.now());
		return true;
	}

	[[nodiscard]] bool onReply(::telemetry::client::Input bytes, std::uint32_t generation,
	                           std::uint32_t requestId) noexcept override
	{
		process(clock_.now());
		if (!ready() || !active_)
			return false;
		const bool consumed = client_.onReply(bytes, generation, requestId);
		settle();
		return consumed;
	}

private:
	static bool finalWrite(const native::RpcClientPacket& packet) noexcept
	{
		return packet.bytes.size() >= ::resource::protocol::wire::writeRequestHeaderSize &&
		       packet.bytes[0] == std::byte(::resource::protocol::Op::Write) &&
		       packet.bytes[13] == std::byte{1};
	}

	static ::telemetry::client::Status translate(native::RpcClientStatus status) noexcept
	{
		switch (status) {
			case native::RpcClientStatus::Ok:
				return ::telemetry::client::Status::Ok;
			case native::RpcClientStatus::Busy:
				return ::telemetry::client::Status::Busy;
			case native::RpcClientStatus::NoSession:
				return ::telemetry::client::Status::NotAdmitted;
			case native::RpcClientStatus::TransactionIdsExhausted:
			case native::RpcClientStatus::RequestIdsExhausted:
				return ::telemetry::client::Status::IdsExhausted;
			case native::RpcClientStatus::BufferTooSmall:
				return ::telemetry::client::Status::BufferTooSmall;
			case native::RpcClientStatus::CodecError:
				return ::telemetry::client::Status::CodecError;
			case native::RpcClientStatus::Timeout:
				return ::telemetry::client::Status::Timeout;
			case native::RpcClientStatus::SessionLost:
				return ::telemetry::client::Status::SessionLost;
			case native::RpcClientStatus::Cancelled:
				return ::telemetry::client::Status::Cancelled;
			case native::RpcClientStatus::MalformedReply:
				return ::telemetry::client::Status::MalformedReply;
			case native::RpcClientStatus::OutcomeUnknown:
				return ::telemetry::client::Status::OutcomeUnknown;
			case native::RpcClientStatus::ResourceError:
				return ::telemetry::client::Status::TransportError;
			default:
				return ::telemetry::client::Status::InvalidArgument;
		}
	}

	void settle(::telemetry::client::Status reason = ::telemetry::client::Status::Ok) noexcept
	{
		if (!active_ || client_.active())
			return;
		const auto& diagnostic = client_.diagnostic();
		outcome_ = {::telemetry::client::Status::Ok,
		            operation_,
		            static_cast<std::uint8_t>(diagnostic.dispatch),
		            diagnostic.endpointStatus,
		            static_cast<std::uint8_t>(diagnostic.codec),
		            {}};
		if (client_.completed()) {
			outcome_.payload = client_.resultBytes();
		} else {
			const bool uncertain =
			    finalPrepared_ || diagnostic.status == native::RpcClientStatus::OutcomeUnknown;
			const auto cause = diagnostic.status == native::RpcClientStatus::OutcomeUnknown
			                       ? diagnostic.cause
			                       : diagnostic.status;
			outcome_.status = reason == ::telemetry::client::Status::Ok ? translate(cause) : reason;
			if (uncertain && operation_ != ::telemetry::client::Operation::FieldRead)
				outcome_.status = ::telemetry::client::Status::OutcomeUnknown;
			if (uncertain)
				requireRecovery();
		}
		active_ = false;
		terminal_ = true;
	}

	void requireRecovery() noexcept
	{
		if (recovery_)
			return;
		recovery_ = true;
		opened_ = false;
		if (session_ != 0)
			client_.loseSession(session_);
	}

	native::RpcClient& client_;
	::telemetry::client::Clock clock_;
	::telemetry::client::ResultView outcome_{};
	::telemetry::client::Operation operation_ = ::telemetry::client::Operation::FieldRead;
	std::uint32_t session_ = 0, preparedRequest_ = 0, deadline_ = 0;
	bool opened_ = false, recovery_ = false, active_ = false, terminal_ = false;
	bool finalPrepared_ = false;
};
} // namespace telemetry::integration::rpc
#endif // TELEMETRY_INTEGRATION_RPC_BACKEND_HPP

/* One-owner, complete-packet backend contract. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#ifndef TELEMETRY_LIB_TELEMETRY_CLIENT_BACKEND_HPP
#define TELEMETRY_LIB_TELEMETRY_CLIENT_BACKEND_HPP
#pragma once
#include "ClientResult.hpp"

namespace telemetry::client {
struct RequestView {
	Operation operation;
	std::uint32_t endpointId;
	Input payload;
	std::uint32_t responseBytes;
	// Remaining overall enqueue budget at activation. Direct backend users may
	// leave this at the bounded maximum and use their backend-specific timeout.
	std::uint32_t timeoutMs = maxTimeoutMs;
};

// How the reply to a packet is framed. A Resource reply carries no identity of
// its own: a port matches it by position, and its framing needs the operation.
enum class ReplyFrame : std::uint8_t {
	Encoded,
	Resource
};

struct Packet {
	Input bytes{};
	std::uint32_t session = 0;
	std::uint32_t requestId = 0;
	// Encoded: the complete request; RPC: only final WRITE can dispatch it.
	bool mayExecute = true;
	ReplyFrame reply = ReplyFrame::Encoded;
	// The Resource operation byte when reply is Resource; zero otherwise.
	std::uint8_t resourceOperation = 0;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return !bytes.empty();
	}
};

// No threads, allocation or transport I/O. The owner serializes every method.
// A port must COPY nextPacket bytes into bounded port-owned storage before
// prepareHandoff, or otherwise retain a lease preventing reset/reuse. This
// interface uses the copy contract: the backend may reuse its own bytes after
// a reply, timeout or cancellation; the port owns its copy until TX release.
// prepareHandoff is called immediately BEFORE the first possible external
// transfer. onSendAccepted reports full-packet handoff, never partial progress.
class Backend {
public:
	virtual ~Backend() = default;
	[[nodiscard]] virtual Status start(RequestView, std::uint32_t now) noexcept = 0;
	virtual void process(std::uint32_t now) noexcept = 0;
	[[nodiscard]] virtual bool completed() const noexcept = 0;
	[[nodiscard]] virtual ResultView result() const noexcept = 0;
	virtual void releaseResult() noexcept = 0;
	virtual void cancel(Status reason) noexcept = 0;
	[[nodiscard]] virtual bool ready() const noexcept = 0;
	[[nodiscard]] virtual bool recoveryRequired() const noexcept = 0;
	[[nodiscard]] virtual std::uint32_t session() const noexcept = 0;
	[[nodiscard]] virtual bool openSession(std::uint32_t generation) noexcept = 0;
	[[nodiscard]] virtual bool confirmSessionClosed(std::uint32_t generation) noexcept = 0;
	virtual void close() noexcept = 0;
	[[nodiscard]] virtual Packet nextPacket() noexcept = 0;
	[[nodiscard]] virtual bool prepareHandoff(std::uint32_t session,
	                                          std::uint32_t requestId) noexcept = 0;
	[[nodiscard]] virtual bool onSendAccepted(std::uint32_t session,
	                                          std::uint32_t requestId) noexcept = 0;
	[[nodiscard]] virtual bool onReply(Input bytes, std::uint32_t session,
	                                   std::uint32_t requestId) noexcept = 0;
};
} // namespace telemetry::client
#endif // TELEMETRY_LIB_TELEMETRY_CLIENT_BACKEND_HPP

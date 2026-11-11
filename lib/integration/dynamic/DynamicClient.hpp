/*
 * @file DynamicClient.hpp
 * @brief Owner-only runtime endpoint contracts over the existing bounded client FIFO.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_DYNAMIC_DYNAMIC_CLIENT_HPP
#define TELEMETRY_INTEGRATION_DYNAMIC_DYNAMIC_CLIENT_HPP
#pragma once

#include <client/DeviceClient.hpp>
#include <telemetry/runtime/DynamicValueView.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace telemetry::integration::read_many::detail {
struct ReadManyAccess;
}

namespace telemetry::integration::batch::detail {
struct BatchAccess;
}

namespace telemetry::integration::dynamic {

enum class ValueStatus : std::uint8_t {
	Ok,
	Stale,
	NoValue,
	TypeMismatch,
	InvalidPayload
};

// The original operation outcome is independent of runtime value availability.
// Both operation.payload and value borrow storage only until the callback ends.
struct DynamicResult {
	client::ResultView operation{};
	ValueStatus valueStatus = ValueStatus::Stale;
	runtime::DynamicValueView value{};

	[[nodiscard]] bool hasValue() const noexcept
	{
		return valueStatus == ValueStatus::Ok && value.valid();
	}
};

// Application evidence, not a driver probe. The caller has actually released all
// old TX/DMA borrows, excluded old frames and closed/reset the old peer session,
// including a partially received RPC request. No method establishes those facts.
struct Quiescence {
	std::uint32_t session = 0;
	bool txReleased = false;
	bool oldFramesExcluded = false;
	bool remoteSessionClosed = false;
};

template<std::size_t N = 8, std::size_t PayloadBytes = 4096, std::size_t CallbackBytes = 192>
class DynamicClient;

// Only DynamicClient can issue a binding. All endpoint contracts are copied;
// no name or pointer into a metadata row is retained.
// Bindings borrow the issuing client's identity and must not outlive that object.
class EndpointBinding {
public:
	EndpointBinding() noexcept = default;

	[[nodiscard]] client::Operation operation() const noexcept
	{
		return operation_;
	}

	[[nodiscard]] descriptor::Category category() const noexcept
	{
		return category_;
	}

	[[nodiscard]] std::uint32_t id() const noexcept
	{
		return id_;
	}

	[[nodiscard]] TypeId requestType() const noexcept
	{
		return requestType_;
	}

	[[nodiscard]] TypeId responseType() const noexcept
	{
		return responseType_;
	}

	[[nodiscard]] std::uint32_t requestBytes() const noexcept
	{
		return requestBytes_;
	}

	[[nodiscard]] std::uint32_t responseBytes() const noexcept
	{
		return responseBytes_;
	}

	[[nodiscard]] std::uint32_t session() const noexcept
	{
		return session_;
	}

private:
	const void* client_ = nullptr;
	const runtime::RuntimeSchemaWorkspace* workspace_ = nullptr;
	std::uint64_t epoch_ = 0, schemaGeneration_ = 0;
	std::uint32_t session_ = 0, id_ = 0, requestBytes_ = 0, responseBytes_ = 0;
	TypeId requestType_ = 0, responseType_ = 0;
	client::Operation operation_ = client::Operation::FieldRead;
	descriptor::Category category_ = descriptor::Category::Field;
	std::uint8_t capabilities_ = 0;
	template<std::size_t, std::size_t, std::size_t>
	friend class DynamicClient;
};

struct EndpointResult {
	client::Status status = client::Status::NotAdmitted;
	EndpointBinding endpoint{};

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return status == client::Status::Ok;
	}
};

namespace detail {

struct BytesOnlyCodec {
	static constexpr std::size_t scratchBytes = 1, scratchAlign = 1;
};

// Callable ownership can execute user code too. An enclosing publication or
// callback guard survives nested owner processing until all temporaries retire.
struct CallbackScope {
	bool& active;
	bool previous;

	explicit CallbackScope(bool& value) noexcept : active(value), previous(value)
	{
		active = true;
	}

	~CallbackScope()
	{
		active = previous;
	}

	CallbackScope(const CallbackScope&) = delete;
	CallbackScope& operator=(const CallbackScope&) = delete;
};

struct AdmissionState {
	runtime::RuntimeSchema schema{};
	client::Backend* backend = nullptr;
	std::array<std::byte, descriptor::descriptorHeaderBytes> header{};
	std::uint32_t session = 0;
	bool active = false;
	mutable bool revoked = false;

	[[nodiscard]] bool schemaCurrent() const noexcept
	{
		return active && !revoked && schema.valid();
	}

	[[nodiscard]] bool identityCurrent() const noexcept
	{
		if (!active || revoked)
			return false;
		if (!schema.valid() || !backend || backend->session() != session) {
			revoked = true;
			return false;
		}
		return true;
	}

	[[nodiscard]] bool current() const noexcept
	{
		return identityCurrent() && backend->ready() && !backend->recoveryRequired();
	}
};

// The core sees only this small policy, never RuntimeSchema or its indexes.
struct RuntimeAdmission {
	static constexpr bool allowTyped = false;
	AdmissionState* state = nullptr;

	[[nodiscard]] bool acceptsDescriptor(client::Input header) const noexcept
	{
		return state && state->schemaCurrent() && header.size() == state->header.size() &&
		       std::equal(header.begin(), header.end(), state->header.begin());
	}

	[[nodiscard]] bool current() const noexcept
	{
		return state && state->current();
	}
};

} // namespace detail

// One owner serializes this object, workspace reset/reparse, backend and port.
// The backend is exclusively borrowed. Stop producers, close and process until
// drained before destruction; physical port release is a separate obligation.
// CallbackBytes includes the internal binding snapshot as well as the user
// callable. Oversized callbacks fail the existing DelegateSlot compile-time fit.
template<std::size_t N, std::size_t PayloadBytes, std::size_t CallbackBytes>
class DynamicClient {
	using Core = client::DeviceClient<detail::BytesOnlyCodec, N, PayloadBytes, CallbackBytes, 1, 1,
	                                  client::NoLock, detail::RuntimeAdmission>;

public:
	explicit DynamicClient(client::Backend& backend, client::Clock clock) noexcept
	    : backend_(backend), state_{.backend = &backend},
	      core_(backend, clock, detail::RuntimeAdmission{&state_}), lastSession_(backend.session()),
	      quiescent_(backend.session() == 0 && !backend.ready() && !backend.completed() &&
	                 !backend.recoveryRequired())
	{}

	DynamicClient(const DynamicClient&) = delete;
	DynamicClient& operator=(const DynamicClient&) = delete;
	DynamicClient(DynamicClient&&) = delete;
	DynamicClient& operator=(DynamicClient&&) = delete;
	~DynamicClient() = default;

	// A failed admission neither publishes a binding nor consumes its epoch.
	// Replacing any earlier binding requires explicit quiescence evidence first.
	[[nodiscard]] client::Status admit(const runtime::RuntimeSchema& schema,
	                                   client::Input observedHeader, std::uint32_t session) noexcept
	{
		if (closed_)
			return client::Status::Closed;
		if (backend_.recoveryRequired())
			return client::Status::RecoveryRequired;
		if (callbackActive_ || core_.pending() != 0 || !quiescent_)
			return client::Status::Busy;
		if (!schema.valid())
			return client::Status::NotAdmitted;
		const auto descriptorBytes = schema.descriptor();
		if (observedHeader.size() != state_.header.size() ||
		    descriptorBytes.size() < state_.header.size() ||
		    !std::equal(observedHeader.begin(), observedHeader.end(), descriptorBytes.begin()))
			return client::Status::NotAdmitted;
		if (session == 0 || session <= lastSession_)
			return client::Status::InvalidArgument;
		if (epoch_ == std::numeric_limits<std::uint64_t>::max())
			return client::Status::IdsExhausted;
		const auto previous = state_;
		state_.schema = schema;
		state_.session = session;
		state_.active = true;
		state_.revoked = false;
		std::copy(observedHeader.begin(), observedHeader.end(), state_.header.begin());
		const auto result = core_.admitDescriptor(observedHeader, session);
		if (result != client::Status::Ok) {
			state_ = previous;
			return result;
		}
		++epoch_;
		lastSession_ = session;
		quiescent_ = false;
		return client::Status::Ok;
	}

	[[nodiscard]] EndpointResult bind(client::Operation operation,
	                                  std::uint32_t endpointId) const noexcept
	{
		const auto gate = submissionGate();
		if (gate != client::Status::Ok)
			return {gate, {}};
		descriptor::Category category;
		if (!categoryFor(operation, category))
			return {client::Status::InvalidArgument, {}};
		const auto* endpoint = find(category, endpointId);
		if (!endpoint)
			return {client::Status::InvalidArgument, {}};
		const auto permission = permissionFor(operation, endpoint->capabilities);
		if (permission != client::Status::Ok)
			return {permission, {}};
		EndpointBinding binding;
		binding.client_ = this;
		binding.workspace_ = state_.schema.owner();
		binding.schemaGeneration_ = state_.schema.generation();
		binding.epoch_ = epoch_;
		binding.session_ = state_.session;
		binding.id_ = endpointId;
		binding.operation_ = operation;
		binding.category_ = category;
		binding.capabilities_ = endpoint->capabilities;
		contract(operation, *endpoint, binding.requestType_, binding.responseType_);
		const auto* request = state_.schema.type(binding.requestType_);
		const auto* response = state_.schema.type(binding.responseType_);
		if (!request || !response)
			return {client::Status::InvalidArgument, {}};
		binding.requestBytes_ = request->wireBytes;
		binding.responseBytes_ = response->wireBytes;
		return {client::Status::Ok, binding};
	}

	[[nodiscard]] bool isCurrent(const EndpointBinding& binding) const noexcept
	{
		return isCurrent() && binding.client_ == this && binding.epoch_ == epoch_ &&
		       binding.workspace_ == state_.schema.owner() &&
		       binding.schemaGeneration_ == state_.schema.generation() &&
		       binding.session_ == state_.session;
	}

	[[nodiscard]] bool isCurrent() const noexcept
	{
		return !closed_ && state_.identityCurrent();
	}

	template<class F>
	[[nodiscard]] client::Submission submit(const EndpointBinding& endpoint, client::Input payload,
	                                        F&& callback, std::uint32_t timeoutMs = 30000) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const DynamicResult&>,
		              "Dynamic completion must be noexcept");
		static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>,
		              "Dynamic completion ownership must not throw");
		const auto gate = submissionGate();
		if (gate != client::Status::Ok)
			return {gate, {}};
		if (!isCurrent(endpoint))
			return {client::Status::NotAdmitted, {}};
		if constexpr (std::is_pointer_v<std::remove_reference_t<F>>)
			if (!callback)
				return {client::Status::InvalidArgument, {}};
		if (!contractsMatch(endpoint) || payload.size() != endpoint.requestBytes_)
			return {client::Status::InvalidArgument, {}};
		if (endpoint.requestType_ != 0 && !state_.schema.view(endpoint.requestType_, payload))
			return {client::Status::CodecError, {}};
		// Freeze the request before user callable moves/destructors can reenter.
		// This scope also prevents replacing admission before core reservation.
		detail::CallbackScope publication{callbackActive_};
		const auto frozen = endpoint;
		auto completion = [this, endpoint = frozen, fn = std::forward<F>(callback)](
		                      const client::ResultView& operation) mutable noexcept {
			detail::CallbackScope callback{callbackActive_};
			const auto result = complete(endpoint, operation);
			fn(result);
		};
		const auto capturedGate = submissionGate();
		if (capturedGate != client::Status::Ok)
			return {capturedGate, {}};
		if (!isCurrent(frozen))
			return {client::Status::NotAdmitted, {}};
		// User callable ownership can change caller bytes after the first check.
		// Recheck before the core copies them; its later callable moves follow that copy.
		if (frozen.requestType_ != 0 && !state_.schema.view(frozen.requestType_, payload))
			return {client::Status::CodecError, {}};
		return core_.submitEncoded({frozen.operation_, frozen.id_, payload, frozen.responseBytes_},
		                           std::move(completion), timeoutMs);
	}

	template<class F>
	[[nodiscard]] client::Submission readField(const EndpointBinding& endpoint, F&& callback,
	                                           std::uint32_t timeoutMs = 30000) noexcept
	{
		return submitOperation(client::Operation::FieldRead, endpoint, {},
		                       std::forward<F>(callback), timeoutMs);
	}

	template<class F>
	[[nodiscard]] client::Submission writeField(const EndpointBinding& endpoint,
	                                            client::Input payload, F&& callback,
	                                            std::uint32_t timeoutMs = 30000) noexcept
	{
		return submitOperation(client::Operation::FieldWrite, endpoint, payload,
		                       std::forward<F>(callback), timeoutMs);
	}

	template<class F>
	[[nodiscard]] client::Submission command(const EndpointBinding& endpoint, client::Input payload,
	                                         F&& callback, std::uint32_t timeoutMs = 30000) noexcept
	{
		return submitOperation(client::Operation::Command, endpoint, payload,
		                       std::forward<F>(callback), timeoutMs);
	}

	template<class F>
	[[nodiscard]] client::Submission command(const EndpointBinding& endpoint, F&& callback,
	                                         std::uint32_t timeoutMs = 30000) noexcept
	{
		return command(endpoint, {}, std::forward<F>(callback), timeoutMs);
	}

	template<class F>
	[[nodiscard]] client::Submission service(const EndpointBinding& endpoint, client::Input payload,
	                                         F&& callback, std::uint32_t timeoutMs = 30000) noexcept
	{
		return submitOperation(client::Operation::Service, endpoint, payload,
		                       std::forward<F>(callback), timeoutMs);
	}

	template<class F>
	[[nodiscard]] client::Submission service(const EndpointBinding& endpoint, F&& callback,
	                                         std::uint32_t timeoutMs = 30000) noexcept
	{
		return service(endpoint, {}, std::forward<F>(callback), timeoutMs);
	}

	// Revocation is synchronous and callback-free; owner processing performs drain.
	void invalidate() noexcept
	{
		state_.active = false;
		core_.invalidateAdmission();
	}

	// A proof cannot be supplied from a callback or while any request is occupied.
	// Closing here is local bookkeeping AFTER the caller's remote/driver cleanup.
	[[nodiscard]] bool confirmQuiescence(Quiescence proof) noexcept
	{
		if (callbackActive_ || core_.pending() != 0 || !proof.txReleased ||
		    !proof.oldFramesExcluded || !proof.remoteSessionClosed ||
		    proof.session != backend_.session() || (state_.active && state_.current()))
			return false;
		backend_.close();
		if (backend_.recoveryRequired() && !core_.confirmSessionClosed(proof.session))
			return false;
		state_.active = false;
		quiescent_ = true;
		return true;
	}

	void process(std::uint32_t now) noexcept
	{
		core_.process(now);
	}

	[[nodiscard]] client::Packet nextPacket() noexcept
	{
		return core_.nextPacket();
	}

	[[nodiscard]] bool prepareHandoff(std::uint32_t session, std::uint32_t requestId) noexcept
	{
		return core_.prepareHandoff(session, requestId);
	}

	[[nodiscard]] bool onSendAccepted(std::uint32_t session, std::uint32_t requestId) noexcept
	{
		return core_.onSendAccepted(session, requestId);
	}

	[[nodiscard]] bool onReply(client::Input bytes, std::uint32_t session,
	                           std::uint32_t requestId) noexcept
	{
		return core_.onReply(bytes, session, requestId);
	}

	[[nodiscard]] bool cancel(client::RequestHandle handle) noexcept
	{
		return core_.cancel(handle);
	}

	[[nodiscard]] std::size_t pending() const noexcept
	{
		return core_.pending();
	}

	[[nodiscard]] bool recoveryRequired() const noexcept
	{
		return backend_.recoveryRequired();
	}

	[[nodiscard]] std::uint32_t session() const noexcept
	{
		return state_.session;
	}

	[[nodiscard]] bool drained() const noexcept
	{
		return core_.drained();
	}

	void close() noexcept
	{
		closed_ = true;
		state_.active = false;
		core_.close();
	}

	// A raw Resource List, Stat or Read for the device's files on this client's
	// queue, under the same admission as every other request. No endpoint and
	// no schema are involved; the caller parses the reply inside its callback.
	template<class F>
	[[nodiscard]] client::Submission submitResource(client::Input request, std::uint32_t replyBytes,
	                                                F&& callback,
	                                                std::uint32_t timeoutMs = 30000) noexcept
	{
		const auto gate = submissionGate();
		if (gate != client::Status::Ok)
			return {gate, {}};
		return core_.submitResource(request, replyBytes, std::forward<F>(callback), timeoutMs);
	}

private:
	friend struct ::telemetry::integration::read_many::detail::ReadManyAccess;
	friend struct ::telemetry::integration::batch::detail::BatchAccess;

	[[nodiscard]] client::Status submissionGate() const noexcept
	{
		if (closed_)
			return client::Status::Closed;
		if (backend_.recoveryRequired())
			return client::Status::RecoveryRequired;
		return state_.current() ? client::Status::Ok : client::Status::NotAdmitted;
	}

	[[nodiscard]] static bool categoryFor(client::Operation operation,
	                                      descriptor::Category& category) noexcept
	{
		switch (operation) {
			case client::Operation::FieldRead:
			case client::Operation::FieldWrite:
				category = descriptor::Category::Field;
				return true;
			case client::Operation::Command:
				category = descriptor::Category::Command;
				return true;
			case client::Operation::Service:
				category = descriptor::Category::Service;
				return true;
			case client::Operation::ReadMany:
			case client::Operation::Batch:
			case client::Operation::Resource:
				return false;
		}
		return false;
	}

	[[nodiscard]] static client::Status permissionFor(client::Operation operation,
	                                                  std::uint8_t capabilities) noexcept
	{
		if (operation == client::Operation::FieldWrite &&
		    !(capabilities & static_cast<std::uint8_t>(descriptor::Capability::Writable)))
			return client::Status::ReadOnly;
		if (operation == client::Operation::FieldRead &&
		    !(capabilities & static_cast<std::uint8_t>(descriptor::Capability::Readable)))
			return client::Status::InvalidArgument;
		return client::Status::Ok;
	}

	static void contract(client::Operation operation, const runtime::RuntimeEndpoint& endpoint,
	                     TypeId& request, TypeId& response) noexcept
	{
		request = operation == client::Operation::FieldRead ? 0 : endpoint.typeId;
		response = operation == client::Operation::FieldRead ? endpoint.typeId
		           : operation == client::Operation::Service ? endpoint.responseTypeId
		                                                     : 0;
	}

	[[nodiscard]] const runtime::RuntimeEndpoint* find(descriptor::Category category,
	                                                   std::uint32_t id) const noexcept
	{
		for (const auto& endpoint : state_.schema.endpoints(category))
			if (endpoint.id == id)
				return &endpoint;
		return nullptr;
	}

	[[nodiscard]] bool contractsMatch(const EndpointBinding& binding) const noexcept
	{
		const auto* endpoint = find(binding.category_, binding.id_);
		if (!endpoint || endpoint->capabilities != binding.capabilities_ ||
		    permissionFor(binding.operation_, endpoint->capabilities) != client::Status::Ok)
			return false;
		TypeId request, response;
		contract(binding.operation_, *endpoint, request, response);
		const auto* requestType = state_.schema.type(request);
		const auto* responseType = state_.schema.type(response);
		return request == binding.requestType_ && response == binding.responseType_ &&
		       requestType && responseType && requestType->wireBytes == binding.requestBytes_ &&
		       responseType->wireBytes == binding.responseBytes_;
	}

	[[nodiscard]] DynamicResult complete(const EndpointBinding& endpoint,
	                                     const client::ResultView& operation) const noexcept
	{
		DynamicResult result{operation, ValueStatus::Stale, {}};
		if (!isCurrent(endpoint))
			return result;
		result.valueStatus = ValueStatus::NoValue;
		if (!operation.succeeded())
			return result;
		if (operation.operation != endpoint.operation_ || !contractsMatch(endpoint)) {
			result.valueStatus = ValueStatus::TypeMismatch;
			return result;
		}
		if (endpoint.responseType_ == 0)
			return result;
		const auto value = state_.schema.view(endpoint.responseType_, operation.payload);
		if (!value) {
			result.valueStatus = ValueStatus::InvalidPayload;
			return result;
		}
		result.valueStatus = ValueStatus::Ok;
		result.value = value.value;
		return result;
	}

	template<class F>
	client::Submission submitOperation(client::Operation operation, const EndpointBinding& endpoint,
	                                   client::Input payload, F&& callback,
	                                   std::uint32_t timeoutMs) noexcept
	{
		const auto gate = submissionGate();
		if (gate != client::Status::Ok)
			return {gate, {}};
		if (endpoint.operation_ != operation)
			return {client::Status::InvalidArgument, {}};
		return submit(endpoint, payload, std::forward<F>(callback), timeoutMs);
	}

	client::Backend& backend_;
	detail::AdmissionState state_;
	Core core_;
	std::uint64_t epoch_ = 0;
	std::uint32_t lastSession_ = 0;
	bool closed_ = false, callbackActive_ = false, quiescent_ = true;
};

} // namespace telemetry::integration::dynamic
#endif // TELEMETRY_INTEGRATION_DYNAMIC_DYNAMIC_CLIENT_HPP

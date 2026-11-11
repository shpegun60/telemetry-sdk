/* Cooperative, fixed-storage asynchronous client. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#ifndef TELEMETRY_LIB_TELEMETRY_CLIENT_DEVICECLIENT_HPP
#define TELEMETRY_LIB_TELEMETRY_CLIENT_DEVICECLIENT_HPP
#pragma once
#include "RequestQueue.hpp"
#include <algorithm>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace telemetry::client {
// Admission stays independent of runtime metadata. Optional integration policies
// may supply a different exact descriptor check and an owner-only execution gate.
template<class Codec>
struct StaticAdmission {
	static constexpr bool allowTyped = true;

	[[nodiscard]] bool acceptsDescriptor(Input header) const noexcept
	{
		const auto expected = Codec::descriptorHeader();
		return expected.size() == 64 && header.size() == expected.size() &&
		       std::equal(header.begin(), header.end(), expected.begin());
	}

	[[nodiscard]] constexpr bool current() const noexcept
	{
		return true;
	}
};

// Codec supplies wireSize<T>, encode/decode returning byte status (zero=Ok),
// descriptorHeader(), scratchBytes and scratchAlign. Generated ClientBindings
// provide this policy using the standalone SDK, without Model or native codec.
template<class Codec, std::size_t N = 8, std::size_t PayloadBytes = 4096,
         std::size_t CallbackBytes = 64, std::size_t ScratchBytes = Codec::scratchBytes,
         std::size_t ScratchAlign = Codec::scratchAlign, class Lock = NoLock,
         class AdmissionPolicy = StaticAdmission<Codec>>
class DeviceClient {
	using Queue = RequestQueue<N, PayloadBytes, CallbackBytes, Lock>;
	using Record = typename Queue::Record;

public:
	using LockPolicy = Lock;

	explicit DeviceClient(Backend& backend, Clock clock, AdmissionPolicy admission = {}) noexcept
	    : backend_(backend), clock_(clock), admission_(admission)
	{
		if (!clock_.read)
			std::abort();
	}

	DeviceClient(const DeviceClient&) = delete;
	DeviceClient& operator=(const DeviceClient&) = delete;
	// Destruction requires stopped producers, drained()==true and no executing
	// callback/process. close() is an asynchronous request, not a destructor drain.
	~DeviceClient() = default;

	[[nodiscard]] Status admitDescriptor(Input header, std::uint32_t generation) noexcept
	{
		if (backend_.recoveryRequired())
			return Status::RecoveryRequired;
		const auto admission = queue_.beginAdmission();
		if (admission != Status::Ok)
			return admission;
		if (!admission_.acceptsDescriptor(header))
			return Status::NotAdmitted;
		if (!backend_.openSession(generation))
			return backend_.recoveryRequired() ? Status::RecoveryRequired : Status::InvalidArgument;
		return queue_.admit(generation) ? Status::Ok : Status::Busy;
	}

	[[nodiscard]] bool confirmSessionClosed(std::uint32_t generation) noexcept
	{
		return queue_.empty() && backend_.confirmSessionClosed(generation);
	}

	// Owner-only revocation of enqueue, activation and new handoffs, independent
	// of the policy. Does not advance the backend or invoke callbacks. Previously
	// authorized effects retain passive completion and the existing deadline;
	// draining and physical transport release remain separate steps.
	void invalidateAdmission() noexcept
	{
		queue_.invalidate(backend_.recoveryRequired() ? Status::RecoveryRequired
		                                              : Status::NotAdmitted);
	}

	[[nodiscard]] std::size_t pending() const noexcept
	{
		return queue_.size();
	}

	// Owner-only lookup for transport/application token correlation.
	[[nodiscard]] RequestHandle activeHandle() const noexcept
	{
		return active_ ? queue_.handle(*active_) : RequestHandle{};
	}

	[[nodiscard]] bool recoveryRequired() const noexcept
	{
		return backend_.recoveryRequired();
	}

	// Owner-only observations for optional operation adapters. These never
	// advance a backend, reserve queue space or invoke a completion callback.
	[[nodiscard]] Status submissionStatus() const noexcept
	{
		if (queue_.closing())
			return Status::Closed;
		if (backend_.recoveryRequired())
			return Status::RecoveryRequired;
		if (!queue_.admitted() || !admission_.current())
			return Status::NotAdmitted;
		return Status::Ok;
	}

	[[nodiscard]] bool usesBackend(const Backend& backend) const noexcept
	{
		return &backend_ == &backend;
	}

	[[nodiscard]] std::uint32_t session() const noexcept
	{
		return backend_.session();
	}

	[[nodiscard]] bool cancel(RequestHandle handle) noexcept
	{
		return queue_.cancel(handle);
	}

	void close() noexcept
	{
		queue_.close();
	}

	[[nodiscard]] bool drained() const noexcept
	{
		return queue_.drained();
	}

	// Owner transport entry points recheck the OVERALL enqueue deadline with a
	// fresh clock immediately before authorizing a port-owned packet copy.
	[[nodiscard]] Packet nextPacket() noexcept
	{
		if (processing_)
			return {};
		process(clock_.now());
		return queue_.admitted() && admission_.current() && active_ && !queue_.closing() &&
		               !queue_.cancelled(*active_)
		           ? backend_.nextPacket()
		           : Packet{};
	}

	[[nodiscard]] bool prepareHandoff(std::uint32_t session, std::uint32_t requestId) noexcept
	{
		if (processing_)
			return false;
		process(clock_.now());
		if (!queue_.admitted() || !admission_.current() || !active_ || queue_.closing() ||
		    queue_.cancelled(*active_) || expired(clock_.now(), active_->deadline))
			return false;
		const auto packet = backend_.nextPacket();
		if (!packet || packet.session != session || packet.requestId != requestId ||
		    !queue_.admitted() || !admission_.current() ||
		    !backend_.prepareHandoff(session, requestId))
			return false;
		activeMayExecute_ = activeMayExecute_ || packet.mayExecute;
		return true;
	}

	[[nodiscard]] bool onSendAccepted(std::uint32_t session, std::uint32_t requestId) noexcept
	{
		if (processing_)
			return false;
		const bool accepted = backend_.onSendAccepted(session, requestId);
		process(clock_.now());
		return accepted;
	}

	[[nodiscard]] bool onReply(Input bytes, std::uint32_t session, std::uint32_t requestId) noexcept
	{
		if (processing_)
			return false;
		process(clock_.now());
		const bool consumed = backend_.onReply(bytes, session, requestId);
		process(clock_.now());
		return consumed;
	}

	template<class E, class F>
	    requires(AdmissionPolicy::allowTyped && static_cast<unsigned>(E::category) == 1)
	[[nodiscard]] Submission read(E endpoint, F&& callback,
	                              std::uint32_t timeoutMs = 30000) noexcept
	{
		if (!endpoint.readable())
			return {Status::InvalidArgument, {}};
		if (!matchesEndpoint(endpoint))
			return {Status::NotAdmitted, {}};
		return enqueue<void, typename E::Value>(Operation::FieldRead, endpoint.id, nullptr,
		                                        std::forward<F>(callback), timeoutMs);
	}

	template<class E, class F>
	    requires(AdmissionPolicy::allowTyped && static_cast<unsigned>(E::category) == 1)
	[[nodiscard]] Submission write(E endpoint, const typename E::Value& value, F&& callback,
	                               std::uint32_t timeoutMs = 30000) noexcept
	{
		if (!endpoint.writable())
			return {Status::ReadOnly, {}};
		if (!matchesEndpoint(endpoint))
			return {Status::NotAdmitted, {}};
		return enqueue<typename E::Value, void>(Operation::FieldWrite, endpoint.id, &value,
		                                        std::forward<F>(callback), timeoutMs);
	}

	template<class E, class F>
	    requires(AdmissionPolicy::allowTyped && static_cast<unsigned>(E::category) == 2 &&
	             !std::is_void_v<typename E::Request>)
	[[nodiscard]] Submission command(E endpoint, const typename E::Request& value, F&& callback,
	                                 std::uint32_t timeoutMs = 30000) noexcept
	{
		if (!matchesEndpoint(endpoint))
			return {Status::NotAdmitted, {}};
		return enqueue<typename E::Request, void>(Operation::Command, endpoint.id, &value,
		                                          std::forward<F>(callback), timeoutMs);
	}

	template<class E, class F>
	    requires(AdmissionPolicy::allowTyped && static_cast<unsigned>(E::category) == 2 &&
	             std::is_void_v<typename E::Request>)
	[[nodiscard]] Submission command(E endpoint, F&& callback,
	                                 std::uint32_t timeoutMs = 30000) noexcept
	{
		if (!matchesEndpoint(endpoint))
			return {Status::NotAdmitted, {}};
		return enqueue<void, void>(Operation::Command, endpoint.id, nullptr,
		                           std::forward<F>(callback), timeoutMs);
	}

	template<class E, class F>
	    requires(AdmissionPolicy::allowTyped && static_cast<unsigned>(E::category) == 3 &&
	             !std::is_void_v<typename E::Request>)
	[[nodiscard]] Submission service(E endpoint, const typename E::Request& value, F&& callback,
	                                 std::uint32_t timeoutMs = 30000) noexcept
	{
		if (!matchesEndpoint(endpoint))
			return {Status::NotAdmitted, {}};
		return enqueue<typename E::Request, typename E::Response>(
		    Operation::Service, endpoint.id, &value, std::forward<F>(callback), timeoutMs);
	}

	template<class E, class F>
	    requires(AdmissionPolicy::allowTyped && static_cast<unsigned>(E::category) == 3 &&
	             std::is_void_v<typename E::Request>)
	[[nodiscard]] Submission service(E endpoint, F&& callback,
	                                 std::uint32_t timeoutMs = 30000) noexcept
	{
		if (!matchesEndpoint(endpoint))
			return {Status::NotAdmitted, {}};
		return enqueue<void, typename E::Response>(Operation::Service, endpoint.id, nullptr,
		                                           std::forward<F>(callback), timeoutMs);
	}

	// QML/application adapters already own generated endpoint lookup and value
	// conversion. They use this SAME bounded queue, never another execution queue.
	template<class F>
	[[nodiscard]] Submission submitEncoded(RequestView request, F&& callback,
	                                       std::uint32_t timeoutMs = 30000) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const ResultView&>);
		static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>,
		              "Completion ownership must be constructed without exceptions");
		if (!validTimeout(timeoutMs))
			return {Status::InvalidArgument, {}};
		// A function reference is never null; check the argument's pointer type.
		if constexpr (std::is_pointer_v<std::remove_reference_t<F>>)
			if (!callback)
				return {Status::InvalidArgument, {}};
		if (!admission_.current())
			return {queue_.closing()              ? Status::Closed
			        : backend_.recoveryRequired() ? Status::RecoveryRequired
			                                      : Status::NotAdmitted,
			        {}};
		const auto reserved = queue_.reserve(request.payload.size());
		if (!reserved.record)
			return {reserved.status, {}};
		auto& record = *reserved.record;
		initialize(record, request.operation, request.endpointId, request.responseBytes, timeoutMs);
		std::copy(request.payload.begin(), request.payload.end(), queue_.payload(record).begin());
		record.callback.bind(
		    [fn = std::forward<F>(callback)](const ResultView& result, Output) mutable noexcept {
			    fn(result);
		    });
		return publish(record);
	}

	// A raw Resource List, Stat or Read for the device's files on this same
	// queue (see Operation::Resource). The request bytes are copied; the reply
	// is the result payload, borrowed during the callback only.
	template<class F>
	[[nodiscard]] Submission submitResource(Input request, std::uint32_t replyBytes, F&& callback,
	                                        std::uint32_t timeoutMs = 30000) noexcept
	{
		return submitEncoded({Operation::Resource, 0, request, replyBytes},
		                     std::forward<F>(callback), timeoutMs);
	}

	// Exactly one completion at most per call. No waiting for I/O, producer
	// reservations or callbacks. User callbacks must obey the application's own
	// timing bound. All backend/lifecycle methods run in this one owner context.
	void process(std::uint32_t now) noexcept
	{
		if (processing_)
			return;
		processing_ = true;
		backend_.process(now);
		if (backend_.recoveryRequired())
			queue_.invalidate(Status::RecoveryRequired);
		else if (!admission_.current() || (!backend_.ready() && active_ == nullptr))
			queue_.invalidate();
		if (active_) {
			if (!backend_.completed()) {
				if (queue_.closing() || queue_.cancelled(*active_))
					backend_.cancel(Status::Cancelled);
				else if (expired(now, active_->deadline))
					backend_.cancel(Status::Timeout);
				else if ((!queue_.admitted() || !admission_.current()) && !activeMayExecute_)
					backend_.cancel(Status::Cancelled);
			}
			if (backend_.completed()) {
				auto* record = active_;
				active_ = nullptr;
				activeMayExecute_ = false;
				if (backend_.recoveryRequired())
					queue_.invalidate(Status::RecoveryRequired);
				complete(*record, backend_.result());
				backend_.releaseResult();
			} else if (auto* record =
			               queue_.takeExpired(now, queue_.closing() || !queue_.admitted())) {
				const auto status =
				    queue_.closing() || !queue_.admitted() || queue_.cancelled(*record)
				        ? Status::Cancelled
				        : Status::Timeout;
				complete(*record, {status, record->operation});
			}
		} else if (auto* expiredRecord =
		               queue_.takeExpired(now, queue_.closing() || !queue_.admitted())) {
			const auto status =
			    queue_.closing() || !queue_.admitted() || queue_.cancelled(*expiredRecord)
			        ? Status::Cancelled
			        : Status::Timeout;
			complete(*expiredRecord, {status, expiredRecord->operation});
		} else if (auto* record = queue_.takeNext()) {
			Status status = Status::Ok;
			if (queue_.closing() || !queue_.admitted() || queue_.cancelled(*record) ||
			    !admission_.current())
				status = Status::Cancelled;
			else if (expired(now, record->deadline))
				status = Status::Timeout;
			if (status == Status::Ok) {
				status =
				    backend_.start({record->operation, record->endpoint, queue_.payload(*record),
				                    record->responseBytes, record->deadline - now},
				                   now);
				if (status == Status::Ok) {
					active_ = record;
					activeMayExecute_ = false;
				}
			}
			if (status != Status::Ok)
				complete(*record, {status, record->operation});
		}
		if (queue_.closing() && queue_.empty() && !backendClosed_) {
			backend_.close();
			backendClosed_ = true;
		}
		processing_ = false;
		if (backendClosed_)
			queue_.markDrained();
	}

private:
	template<class E>
	static bool matchesEndpoint(E endpoint) noexcept
	{
		if constexpr (requires {
			              endpoint.fingerprint;
			              Codec::fingerprint;
		              })
			return endpoint.fingerprint == Codec::fingerprint;
		else
			return true;
	}

	static constexpr bool validTimeout(std::uint32_t timeout) noexcept
	{
		return timeout && timeout <= maxTimeoutMs;
	}

	void initialize(Record& record, Operation operation, std::uint32_t endpoint,
	                std::uint32_t response, std::uint32_t timeout) noexcept
	{
		record.operation = operation;
		record.endpoint = endpoint;
		record.responseBytes = response;
		record.deadline = clock_.now() + timeout;
	}

	[[nodiscard]] Submission publish(Record& record) noexcept
	{
		auto submitted = queue_.publish(record);
		if (!submitted)
			queue_.release(record);
		return submitted;
	}

	template<class Q, class R, class F>
	Submission enqueue(Operation operation, std::uint32_t endpoint, const Q* value, F&& callback,
	                   std::uint32_t timeoutMs) noexcept
	{
		using Callback = std::decay_t<F>;
		static_assert(std::is_nothrow_constructible_v<Callback, F&&>,
		              "Completion ownership must be constructed without exceptions");
		static_assert(std::is_nothrow_invocable_v<Callback&, const Result<R>&>,
		              "Completion must be noexcept");
		if (!validTimeout(timeoutMs))
			return {Status::InvalidArgument, {}};
		if constexpr (std::is_pointer_v<std::remove_reference_t<F>>)
			if (!callback)
				return {Status::InvalidArgument, {}};
		if constexpr (!std::is_void_v<R>) {
			static_assert(sizeof(R) <= ScratchBytes && alignof(R) <= ScratchAlign,
			              "Response exceeds the single DTO scratch");
			static_assert(std::is_nothrow_default_constructible_v<R> &&
			              std::is_nothrow_destructible_v<R>);
		}
		const auto reserved = queue_.reserve(Codec::template wireSize<Q>);
		if (!reserved.record)
			return {reserved.status, {}};
		auto& record = *reserved.record;
		initialize(record, operation, endpoint, Codec::template wireSize<R>, timeoutMs);
		if constexpr (!std::is_void_v<Q>) {
			if (Codec::encode(*value, queue_.payload(record)) != 0) {
				queue_.release(record);
				return {Status::CodecError, {}};
			}
		}
		record.callback.bind([fn = std::forward<F>(callback)](const ResultView& view,
		                                                      Output scratch) mutable noexcept {
			Result<R> result{};
			static_cast<ResultView&>(result) = view;
			if constexpr (std::is_void_v<R>) {
				fn(result);
			} else {
				R* object = nullptr;
				if (result.succeeded()) {
					object = ::new (static_cast<void*>(scratch.data())) R;
					result.codec = Codec::decode(view.payload, *object);
					if (result.codec != 0)
						result.status = Status::CodecError;
					else
						result.object = object;
				}
				fn(result);
				if (object)
					std::destroy_at(object);
			}
		});
		return publish(record);
	}

	void complete(Record& record, ResultView result) noexcept
	{
		queue_.completing(record);
		record.callback.invoke(result, scratch_);
		queue_.release(record);
	}

	Backend& backend_;
	Clock clock_;
	[[no_unique_address]] AdmissionPolicy admission_;
	Queue queue_{};
	alignas(ScratchAlign) std::array<std::byte, ScratchBytes> scratch_{};
	Record* active_ = nullptr;
	bool processing_ = false, backendClosed_ = false, activeMayExecute_ = false;
};
} // namespace telemetry::client
#endif // TELEMETRY_LIB_TELEMETRY_CLIENT_DEVICECLIENT_HPP

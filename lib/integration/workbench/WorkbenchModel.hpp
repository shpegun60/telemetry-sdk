/*
 * @file WorkbenchModel.hpp
 * @brief Shared FieldWrite, Command and Service editing over the Inspector's single runtime client.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_WORKBENCH_WORKBENCH_MODEL_HPP
#define TELEMETRY_INTEGRATION_WORKBENCH_WORKBENCH_MODEL_HPP
#pragma once

#include <integration/inspector/InspectorModel.hpp>
#include "RequestEditor.hpp"
#include <common/DelegateSlot.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace telemetry::integration::read_many::detail {
struct WorkbenchAccess;
}

namespace telemetry::integration::batch::detail {
struct WorkbenchAccess;
}

namespace telemetry::integration::files::detail {
struct WorkbenchAccess;
}

namespace telemetry::integration::workbench {

enum class Phase : std::uint8_t {
	Idle,
	Editing,
	Writing,
	Refreshing,
	Executing
};

// These independent outcomes own no response bytes or borrowed views.
struct ApplyResult {
	inspector::OperationResult write{client::Status::Ok, client::Operation::FieldWrite};
	bool writeCompleted = false;
	bool refreshAttempted = false;
	client::Status refreshSubmission = client::Status::InvalidArgument;
	bool refreshSubmitted = false;
	inspector::ReadResult refresh{};
	bool refreshCompleted = false;
};

// Owns only outcome/identity. Response nodes borrow the separate caller-owned
// response store, which is published before this completion is invoked.
struct ServiceResult {
	inspector::OperationResult operation{client::Status::Ok, client::Operation::Service};
	dynamic::ValueStatus valueStatus = dynamic::ValueStatus::Stale;
	inspector::ModelStatus modelStatus = inspector::ModelStatus::Stale;
	std::uint32_t serviceId = 0;
	std::uint64_t revision = 0;
	bool published = false;
};

// One owner serializes the facade, callbacks and schema changes. The private
// Inspector owns the only DynamicClient/FIFO. Draft and committed request bytes
// are separate from its published snapshot. No automatic mutation retry exists.
// A completion callback may reenter read/edit APIs; another apply while that
// callback is executing returns Busy. Destruction requires close/process/drained
// and actual transport release, and must wait until callbacks have returned.
namespace detail {
template<std::size_t N, std::size_t PayloadBytes, std::size_t SnapshotBytes,
         std::size_t CallbackBytes, bool Borrowed>
class WorkbenchState {
public:
	explicit WorkbenchState(client::Backend& backend, client::Clock clock) noexcept
	    requires(!Borrowed)
	    : WorkbenchState(backend, clock, {})
	{}

	// Response storage remains exclusive, disjoint and alive through close/drain.
	// The two-argument constructor reserves no implicit response payload array:
	// Void and zero-byte Struct responses still work without one.
	WorkbenchState(client::Backend& backend, client::Clock clock,
	               std::span<std::byte> responseStorage) noexcept
	    requires(!Borrowed)
	    : model_(backend, clock),
	      requestStore_(request_,
	                    {this,
	                     +[](const void* context) noexcept {
		                     return static_cast<const WorkbenchState*>(context)->model_.isCurrent();
	                     }}),
	      responseStorage_(responseStorage),
	      responseStore_(
	          responseStorage,
	          {this,
	           +[](const void* context) noexcept {
		           return static_cast<const WorkbenchState*>(context)->model_.isCurrent();
	           }}),
	      editor_(draft_, request_)
	{}

	WorkbenchState(WorkbenchState<N, PayloadBytes, SnapshotBytes, CallbackBytes, false>& owner,
	               std::span<std::byte> responseStorage) noexcept
	    requires Borrowed
	    : model_(owner.model_),
	      requestStore_(request_,
	                    {this,
	                     +[](const void* context) noexcept {
		                     return static_cast<const WorkbenchState*>(context)->model_.isCurrent();
	                     }}),
	      responseStorage_(responseStorage),
	      responseStore_(
	          responseStorage,
	          {this,
	           +[](const void* context) noexcept {
		           return static_cast<const WorkbenchState*>(context)->model_.isCurrent();
	           }}),
	      editor_(draft_, request_),
	      responseOwnerOverlap_(runtime::writer_detail::overlaps(
	          responseStorage, runtime::writer_detail::objectBytes(owner)))
	{}

	[[nodiscard]] client::Status synchronize() noexcept
	    requires Borrowed
	{
		if (publishing_ || completionActive_ || !model_.drained())
			return client::Status::Busy;
		if (model_.isCurrent())
			return client::Status::Ok;
		if (!requestStore_.canAdmit() || !responseStore_.canAdmit())
			return client::Status::IdsExhausted;
		const auto status = model_.synchronize();
		if (status != client::Status::Ok)
			return status;
		if (!responseMemoryValid(model_.schema())) {
			model_.snapshot_.invalidate();
			return client::Status::InvalidArgument;
		}
		cancelEdit();
		if (requestStore_.admit(model_.schema(), model_.session()) != inspector::ModelStatus::Ok ||
		    responseStore_.admit(model_.schema(), model_.session()) != inspector::ModelStatus::Ok) {
			model_.snapshot_.invalidate();
			return client::Status::InvalidArgument;
		}
		return client::Status::Ok;
	}

	// Data freshness is local, independent of shared admission. Accepted callbacks
	// retain operation facts but a superseded read cannot republish old data.
	void invalidateData() noexcept
	{
		responseStore_.clearSelection();
		retireEditor();
		model_.invalidateData();
	}

	void retire() noexcept
	    requires Borrowed
	{
		responseStore_.invalidate();
		retireEditor();
		model_.retire();
	}

	WorkbenchState(const WorkbenchState&) = delete;
	WorkbenchState& operator=(const WorkbenchState&) = delete;
	WorkbenchState(WorkbenchState&&) = delete;
	WorkbenchState& operator=(WorkbenchState&&) = delete;

	[[nodiscard]] client::Status admit(const runtime::RuntimeSchema& schema, client::Input header,
	                                   std::uint32_t session) noexcept
	{
		if (publishing_)
			return publicationStatus();
		const auto admissionGate = model_.submissionGate();
		if (admissionGate == client::Status::Closed ||
		    admissionGate == client::Status::RecoveryRequired)
			return admissionGate;
		if (!requestStore_.canAdmit() || !responseStore_.canAdmit()) {
			const auto gate = model_.submissionGate();
			return gate == client::Status::Closed || gate == client::Status::RecoveryRequired
			           ? gate
			           : client::Status::IdsExhausted;
		}
		if (schema.valid() && !responseMemoryValid(schema))
			return client::Status::InvalidArgument;
		const auto status = model_.admit(schema, header, session);
		if (status == client::Status::Ok) {
			cancelEdit();
			if (requestStore_.admit(schema, session) != inspector::ModelStatus::Ok ||
			    responseStore_.admit(schema, session) != inspector::ModelStatus::Ok) {
				model_.invalidate();
				return client::Status::InvalidArgument;
			}
		}
		return status;
	}

	[[nodiscard]] auto catalogs(descriptor::Category category) const noexcept
	{
		return model_.catalogs(category);
	}

	[[nodiscard]] auto endpoints(descriptor::Category category) const noexcept
	{
		return model_.endpoints(category);
	}

	[[nodiscard]] auto fields() const noexcept
	{
		return model_.fields();
	}

	[[nodiscard]] auto commands() const noexcept
	{
		return model_.commands();
	}

	[[nodiscard]] auto services() const noexcept
	{
		return model_.services();
	}

	[[nodiscard]] auto bindField(std::uint32_t id) const noexcept
	{
		return model_.bindField(id);
	}

	[[nodiscard]] client::Status selectField(const dynamic::EndpointBinding& field) noexcept
	{
		if (publishing_)
			return publicationStatus();
		if (phase_ == Phase::Executing)
			return client::Status::Busy;
		const auto status = model_.selectField(field);
		if (status == client::Status::Ok)
			cancelEdit();
		return status;
	}

	void clearSelection() noexcept
	{
		if (publishing_ || phase_ == Phase::Executing)
			return;
		cancelEdit();
		model_.clearSelection();
	}

	template<class F>
	[[nodiscard]] inspector::ReadSubmission readField(const dynamic::EndpointBinding& field,
	                                                  F&& callback,
	                                                  std::uint32_t timeout = 30000) noexcept
	{
		if (publishing_)
			return {publicationStatus(), inspector::ModelStatus::Ok, {}};
		if (phase_ == Phase::Executing)
			return {client::Status::Busy, inspector::ModelStatus::Ok, {}};
		const auto submitted = model_.readField(field, std::forward<F>(callback), timeout);
		if (submitted)
			cancelEdit();
		return submitted;
	}

	template<class F>
	[[nodiscard]] inspector::ReadSubmission refresh(F&& callback,
	                                                std::uint32_t timeout = 30000) noexcept
	{
		if (publishing_)
			return {publicationStatus(), inspector::ModelStatus::Ok, {}};
		if (requestSelected_ || phase_ == Phase::Executing)
			return {client::Status::Busy, inspector::ModelStatus::Ok, {}};
		const auto submitted = model_.refresh(std::forward<F>(callback), timeout);
		if (submitted)
			cancelEdit();
		return submitted;
	}

	[[nodiscard]] auto snapshotInfo() const noexcept
	{
		return model_.snapshotInfo();
	}

	[[nodiscard]] auto root() const noexcept
	{
		return model_.root();
	}

	[[nodiscard]] auto info(const inspector::NodeHandle& node) const noexcept
	{
		return model_.info(node);
	}

	[[nodiscard]] auto childCount(const inspector::NodeHandle& node) const noexcept
	{
		return model_.childCount(node);
	}

	[[nodiscard]] auto child(const inspector::NodeHandle& node, std::uint32_t index) const noexcept
	{
		return model_.child(node, index);
	}

	[[nodiscard]] auto scalar(const inspector::NodeHandle& node) const noexcept
	{
		return model_.scalar(node);
	}

	[[nodiscard]] auto decimal(const inspector::NodeHandle& node,
	                           std::span<char> output) const noexcept
	{
		return model_.decimal(node, output);
	}

	[[nodiscard]] bool isCurrent() const noexcept
	{
		return model_.isCurrent();
	}

	[[nodiscard]] bool isCurrent(const dynamic::EndpointBinding& field) const noexcept
	{
		return model_.isCurrent(field);
	}

	void invalidate() noexcept
	{
		responseStore_.invalidate();
		retireEditor();
		model_.invalidate();
	}

	[[nodiscard]] bool confirmQuiescence(dynamic::Quiescence proof) noexcept
	{
		return !publishing_ && model_.confirmQuiescence(proof);
	}

	void process(std::uint32_t now) noexcept
	{
		if (publishing_)
			return;
		model_.process(now);
		if (phase_ == Phase::Editing && !editCurrent())
			cancelEdit();
	}

	[[nodiscard]] auto nextPacket() noexcept
	{
		return publishing_ ? client::Packet{} : model_.nextPacket();
	}

	[[nodiscard]] bool prepareHandoff(std::uint32_t session, std::uint32_t request) noexcept
	{
		return !publishing_ && model_.prepareHandoff(session, request);
	}

	[[nodiscard]] bool onSendAccepted(std::uint32_t session, std::uint32_t request) noexcept
	{
		return !publishing_ && model_.onSendAccepted(session, request);
	}

	[[nodiscard]] bool onReply(client::Input input, std::uint32_t session,
	                           std::uint32_t request) noexcept
	{
		return !publishing_ && model_.onReply(input, session, request);
	}

	[[nodiscard]] bool cancel(client::RequestHandle request) noexcept
	{
		return !publishing_ && model_.cancel(request);
	}

	[[nodiscard]] std::size_t pending() const noexcept
	{
		return model_.pending();
	}

	[[nodiscard]] bool drained() const noexcept
	{
		return !publishing_ && !completionActive_ && model_.drained();
	}

	[[nodiscard]] bool recoveryRequired() const noexcept
	{
		return model_.recoveryRequired();
	}

	[[nodiscard]] std::uint32_t session() const noexcept
	{
		return model_.session();
	}

	void close() noexcept
	{
		responseStore_.invalidate();
		retireEditor();
		model_.close();
	}

	[[nodiscard]] Phase phase() const noexcept
	{
		return phase_;
	}

	[[nodiscard]] bool editing() const noexcept
	{
		return phase_ == Phase::Editing && editCurrent();
	}

	[[nodiscard]] bool canEdit() const noexcept
	{
		if (publishing_ || requestSelected_ || phase_ != Phase::Idle || !model_.isCurrent())
			return false;
		const auto snapshot = model_.snapshotInfo();
		return snapshot.ready && snapshot.bytes <= PayloadBytes &&
		       bool(model_.client().bind(client::Operation::FieldWrite, snapshot.fieldId));
	}

	// Dirty compares bytes only when requested; scalar updates never copy or
	// recursively validate the whole DTO, and editing back restores dirty=false.
	[[nodiscard]] bool dirty() const noexcept
	{
		if (!editing())
			return false;
		return editor_.dirty();
	}

	[[nodiscard]] bool canApply() const noexcept
	{
		return !publishing_ && !requestSelected_ && editing() && dirty() && !completionActive_;
	}

	[[nodiscard]] runtime::Result<std::uint32_t> copyDraft(client::Output output) const noexcept
	{
		if (publishing_ || completionActive_ || !editing())
			return {runtime::Status::Stale, 0};
		if (runtime::writer_detail::overlaps(output, runtime::writer_detail::objectBytes(*this)))
			return {runtime::Status::Overlap, 0};
		return editor_.copyDraft(output);
	}

	[[nodiscard]] runtime::Status beginEdit() noexcept
	{
		if (publishing_ || requestSelected_ || phase_ != Phase::Idle || !model_.isCurrent())
			return runtime::Status::Stale;
		const auto snapshot = model_.snapshotInfo();
		if (!snapshot.ready)
			return runtime::Status::Stale;
		if (snapshot.bytes > PayloadBytes)
			return runtime::Status::Capacity;
		const auto writable = model_.client().bind(client::Operation::FieldWrite, snapshot.fieldId);
		if (!writable)
			return runtime::Status::TypeMismatch;
		const auto status = editor_.begin(model_.snapshot_);
		if (status != runtime::Status::Ok)
			return status;
		editRead_ = model_.selection_;
		editWrite_ = writable.endpoint;
		phase_ = Phase::Editing;
		return runtime::Status::Ok;
	}

	template<class T>
	[[nodiscard]] runtime::Status setScalar(const inspector::NodeHandle& node,
	                                        const T& value) noexcept
	{
		if (publishing_ || !editing())
			return runtime::Status::Stale;
		return editor_.setScalar(node, value);
	}

	[[nodiscard]] inspector::Result<inspector::ScalarValue>
	editScalar(const inspector::NodeHandle& node) const noexcept
	{
		if (!editing())
			return {inspector::ModelStatus::Stale, {}};
		return editor_.scalar(node);
	}

	void cancelEdit() noexcept
	{
		if (publishing_ || phase_ != Phase::Editing)
			return;
		retireEditor();
	}

	template<class F>
	[[nodiscard]] inspector::ReadSubmission apply(F&& callback, std::uint32_t writeTimeout = 30000,
	                                              std::uint32_t refreshTimeout = 30000) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const ApplyResult&>,
		              "Workbench completion must be noexcept");
		static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>,
		              "Workbench completion ownership must not throw");
		const auto gate = model_.submissionGate();
		if (gate != client::Status::Ok)
			return {gate, inspector::ModelStatus::Ok, {}};
		if (publishing_ || completionActive_ || phase_ == Phase::Writing ||
		    phase_ == Phase::Refreshing || phase_ == Phase::Executing)
			return {client::Status::Busy, inspector::ModelStatus::Ok, {}};
		if (!canApply())
			return {client::Status::InvalidArgument, inspector::ModelStatus::NoSnapshot, {}};
		if (!writeTimeout || writeTimeout > client::maxTimeoutMs || !refreshTimeout ||
		    refreshTimeout > client::maxTimeoutMs)
			return {client::Status::InvalidArgument, inspector::ModelStatus::Ok, {}};
		if constexpr (std::is_pointer_v<std::remove_reference_t<F>>)
			if (callback == nullptr)
				return {client::Status::InvalidArgument, inspector::ModelStatus::Ok, {}};
		const Publication publication{*this};
		const auto committed = editor_.commit();
		if (!committed)
			return {client::Status::CodecError, inspector::ModelStatus::InvalidPayload, {}};
		completion_.bind(
		    [fn = std::forward<F>(callback)](const Completion& result) mutable noexcept {
			    fn(result.field);
		    });
		const auto gateAfterMove = model_.admissionGate();
		const auto capturedGate = gateAfterMove == client::Status::Ok && !editor_.current()
		                              ? client::Status::Cancelled
		                              : gateAfterMove;
		if (capturedGate != client::Status::Ok) {
			completion_.reset();
			(void)editor_.rearm();
			return {capturedGate, inspector::ModelStatus::Ok, {}};
		}
		applyResult_ = {};
		const auto submitted = model_.client().writeField(
		    editWrite_, committed.value,
		    model_.holdCallback(
		        [this, refreshTimeout](const dynamic::DynamicResult& result) noexcept {
			        writeCompleted(result, refreshTimeout);
		        }),
		    writeTimeout);
		if (!submitted) {
			completion_.reset();
			// commit consumed its Writer; retain the committed payload and rebuild
			// the lease so a refused submission neither discards nor freezes edits.
			(void)editor_.rearm();
			return {submitted.status, inspector::ModelStatus::Ok, {}};
		}
		const bool localRevoked = [&]() noexcept {
			if constexpr (Borrowed)
				return model_.admissionGate() != client::Status::Ok || !editor_.current();
			return false;
		}();
		editor_.clear();
		model_.snapshot_.retire();
		responseStore_.clearSelection();
		applyAdmission_ = model_.snapshot_.admission();
		applyRevision_ = model_.snapshot_.revision();
		phase_ = Phase::Writing;
		if (localRevoked)
			(void)model_.client().cancel(submitted.handle);
		return {client::Status::Ok, inspector::ModelStatus::Ok, submitted.handle};
	}

	[[nodiscard]] dynamic::EndpointResult bindCommand(std::uint32_t id) const noexcept
	{
		const auto gate = model_.submissionGate();
		return gate == client::Status::Ok ? model_.client().bind(client::Operation::Command, id)
		                                  : dynamic::EndpointResult{gate, {}};
	}

	[[nodiscard]] client::Status selectCommand(const dynamic::EndpointBinding& command) noexcept
	{
		return selectRequest(command, client::Operation::Command);
	}

	[[nodiscard]] dynamic::EndpointResult bindService(std::uint32_t id) const noexcept
	{
		const auto gate = model_.submissionGate();
		return gate == client::Status::Ok ? model_.client().bind(client::Operation::Service, id)
		                                  : dynamic::EndpointResult{gate, {}};
	}

	[[nodiscard]] client::Status selectService(const dynamic::EndpointBinding& service) noexcept
	{
		return selectRequest(service, client::Operation::Service);
	}

	[[nodiscard]] bool commandSelected() const noexcept
	{
		return requestCurrent() && requestBinding_.operation() == client::Operation::Command;
	}

	[[nodiscard]] bool serviceSelected() const noexcept
	{
		return requestCurrent() && requestBinding_.operation() == client::Operation::Service;
	}

	[[nodiscard]] bool canExecute() const noexcept
	{
		return commandSelected() && canSubmitRequest();
	}

	[[nodiscard]] bool canCallService() const noexcept
	{
		return serviceSelected() && canSubmitRequest() && responseReadyForCall();
	}

	[[nodiscard]] auto editorSnapshotInfo() const noexcept
	{
		return requestCurrent() ? editor_.snapshotInfo() : inspector::SnapshotInfo{};
	}

	[[nodiscard]] auto editorRoot() const noexcept
	{
		return requestCurrent() ? editor_.root()
		                        : inspector::NodeResult{inspector::ModelStatus::Stale, {}};
	}

	[[nodiscard]] auto editorInfo(const inspector::NodeHandle& node) const noexcept
	{
		return requestCurrent()
		           ? editor_.info(node)
		           : inspector::Result<inspector::NodeInfo>{inspector::ModelStatus::Stale, {}};
	}

	[[nodiscard]] auto editorChild(const inspector::NodeHandle& node,
	                               std::uint32_t index) const noexcept
	{
		return requestCurrent() ? editor_.child(node, index)
		                        : inspector::NodeResult{inspector::ModelStatus::Stale, {}};
	}

	[[nodiscard]] auto editorChildCount(const inspector::NodeHandle& node) const noexcept
	{
		return requestCurrent()
		           ? editor_.childCount(node)
		           : inspector::Result<std::uint32_t>{inspector::ModelStatus::Stale, 0};
	}

	template<class F>
	[[nodiscard]] inspector::ReadSubmission execute(F&& callback,
	                                                std::uint32_t timeout = 30000) noexcept
	{
		static_assert(
		    std::is_nothrow_invocable_v<std::decay_t<F>&, const inspector::OperationResult&>,
		    "Command completion must be noexcept");
		return submitRequest<false>(std::forward<F>(callback), timeout);
	}

	template<class F>
	[[nodiscard]] inspector::ReadSubmission callService(F&& callback,
	                                                    std::uint32_t timeout = 30000) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const ServiceResult&>,
		              "Service completion must be noexcept");
		return submitRequest<true>(std::forward<F>(callback), timeout);
	}

	[[nodiscard]] auto serviceSnapshotInfo() const noexcept
	{
		return responseStore_.snapshotInfo();
	}

	[[nodiscard]] auto serviceRoot() const noexcept
	{
		return responseStore_.root();
	}

	[[nodiscard]] auto serviceInfo(const inspector::NodeHandle& node) const noexcept
	{
		return responseStore_.info(node);
	}

	[[nodiscard]] auto serviceChild(const inspector::NodeHandle& node,
	                                std::uint32_t index) const noexcept
	{
		return responseStore_.child(node, index);
	}

	[[nodiscard]] auto serviceChildCount(const inspector::NodeHandle& node) const noexcept
	{
		return responseStore_.childCount(node);
	}

	[[nodiscard]] auto serviceScalar(const inspector::NodeHandle& node) const noexcept
	{
		return responseStore_.scalar(node);
	}

	[[nodiscard]] auto serviceDecimal(const inspector::NodeHandle& node,
	                                  std::span<char> output) const noexcept
	{
		return responseStore_.decimal(node, output);
	}

private:
	friend struct ::telemetry::integration::read_many::detail::WorkbenchAccess;
	friend struct ::telemetry::integration::batch::detail::WorkbenchAccess;
	friend struct ::telemetry::integration::files::detail::WorkbenchAccess;
	template<std::size_t, std::size_t, std::size_t, std::size_t, bool>
	friend class WorkbenchState;

	// Optional read integrations use this same FIFO without exposing it to
	// applications or adding wire dependencies/storage to the Workbench.
	[[nodiscard]] auto& readOnlyClient() noexcept
	{
		return model_.client();
	}

	[[nodiscard]] client::Status readOnlyGate() const noexcept
	{
		return model_.submissionGate();
	}

	inspector::detail::InspectorState<N, PayloadBytes, SnapshotBytes, CallbackBytes, Borrowed>
	    model_;
	std::array<std::byte, PayloadBytes> draft_{}, request_{};
	inspector::SnapshotStore requestStore_;
	std::span<std::byte> responseStorage_;
	inspector::SnapshotStore responseStore_;
	RequestEditor editor_;
	dynamic::EndpointBinding editRead_{}, editWrite_{};
	dynamic::EndpointBinding requestBinding_{};
	std::uint64_t applyAdmission_ = 0, applyRevision_ = 0, serviceRevision_ = 0;
	Phase phase_ = Phase::Idle;
	bool completionActive_ = false, requestSelected_ = false, publishing_ = false;
	bool responseOwnerOverlap_ = false;

	// Owning a callable may run its user-defined move/destructor. Keep request,
	// binding and completion publication indivisible to those owner reentries.
	// Revocation remains allowed; no callbacks or transport progress occur here.
	struct Publication {
		dynamic::detail::CallbackScope active, owner;

		explicit Publication(WorkbenchState& self) noexcept
		    : active(self.publishing_), owner(self.model_.ownerPublishing())
		{}
	};

	struct Completion {
		ApplyResult field{};
		inspector::OperationResult command{};
		ServiceResult service{};
	};

	DelegateSlot<void(const Completion&) noexcept, CallbackBytes> completion_;
	ApplyResult applyResult_{};

	[[nodiscard]] bool responseMemoryValid(const runtime::RuntimeSchema& schema) const noexcept
	{
		return schema.valid() && !responseOwnerOverlap_ &&
		       !runtime::writer_detail::overlaps(responseStorage_,
		                                         runtime::writer_detail::objectBytes(*this)) &&
		       !runtime::writer_detail::overlaps(responseStorage_,
		                                         runtime::writer_detail::objectBytes(schema)) &&
		       !runtime::writer_detail::overlapsSchema(schema, responseStorage_);
	}

	[[nodiscard]] bool requestCurrent() const noexcept
	{
		return requestSelected_ && model_.isCurrent(requestBinding_);
	}

	[[nodiscard]] bool canSubmitRequest() const noexcept
	{
		return !publishing_ && phase_ == Phase::Editing && !completionActive_ &&
		       model_.submissionGate() == client::Status::Ok && editor_.current();
	}

	[[nodiscard]] bool responseReadyForCall() const noexcept
	{
		return requestBinding_.responseBytes() <= responseStorage_.size() &&
		       responseStore_.canAdvance() && responseMemoryValid(model_.schema());
	}

	[[nodiscard]] client::Status selectRequest(const dynamic::EndpointBinding& binding,
	                                           client::Operation operation) noexcept
	{
		const auto gate = model_.submissionGate();
		if (gate != client::Status::Ok)
			return gate;
		if (publishing_ || completionActive_ || phase_ == Phase::Writing ||
		    phase_ == Phase::Refreshing || phase_ == Phase::Executing ||
		    (phase_ == Phase::Editing && !requestSelected_))
			return client::Status::Busy;
		if (!model_.client().isCurrent(binding))
			return client::Status::NotAdmitted;
		if (binding.operation() != operation)
			return client::Status::InvalidArgument;
		const auto rebound = model_.client().bind(operation, binding.id());
		if (!rebound)
			return rebound.status;
		if (binding.requestBytes() > request_.size())
			return client::Status::BufferTooSmall;
		if (!requestStore_.canAdvance())
			return client::Status::IdsExhausted;
		const auto initialized =
		    RequestEditor::initialize(model_.schema(), binding.requestType(),
		                              std::span{request_}.first(binding.requestBytes()));
		if (initialized != runtime::Status::Ok)
			return client::Status::CodecError;
		editor_.clear();
		const auto selected =
		    requestStore_.selectEndpoint(binding.id(), binding.requestType(), binding.category());
		if (selected != inspector::ModelStatus::Ok)
			return client::Status::NotAdmitted;
		if (binding.requestType() != 0 &&
		    requestStore_.publish(client::Input{request_}.first(binding.requestBytes()),
		                          requestStore_.admission(),
		                          requestStore_.revision()) != inspector::ModelStatus::Ok)
			return client::Status::CodecError;
		if (editor_.begin(requestStore_) != runtime::Status::Ok)
			return client::Status::CodecError;
		requestBinding_ = binding;
		requestSelected_ = true;
		phase_ = Phase::Editing;
		return client::Status::Ok;
	}

	template<bool Service, class F>
	[[nodiscard]] inspector::ReadSubmission submitRequest(F&& callback,
	                                                      std::uint32_t timeout) noexcept
	{
		static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>,
		              "Workbench completion ownership must not throw");
		const auto gate = model_.submissionGate();
		if (gate != client::Status::Ok)
			return {gate, inspector::ModelStatus::Ok, {}};
		if (publishing_ || completionActive_ || phase_ == Phase::Writing ||
		    phase_ == Phase::Refreshing || phase_ == Phase::Executing)
			return {client::Status::Busy, inspector::ModelStatus::Ok, {}};
		if (!(Service ? serviceSelected() : commandSelected()) || !canSubmitRequest())
			return {client::Status::InvalidArgument, inspector::ModelStatus::NoSelection, {}};
		if (!timeout || timeout > client::maxTimeoutMs)
			return {client::Status::InvalidArgument, inspector::ModelStatus::Ok, {}};
		if constexpr (std::is_pointer_v<std::remove_reference_t<F>>)
			if (callback == nullptr)
				return {client::Status::InvalidArgument, inspector::ModelStatus::Ok, {}};
		if constexpr (Service) {
			if (requestBinding_.responseBytes() > responseStorage_.size())
				return {
				    client::Status::BufferTooSmall, inspector::ModelStatus::SnapshotTooLarge, {}};
			if (!responseStore_.canAdvance())
				return {
				    client::Status::IdsExhausted, inspector::ModelStatus::RevisionExhausted, {}};
			if (!responseMemoryValid(model_.schema()))
				return {
				    client::Status::InvalidArgument, inspector::ModelStatus::InvalidPayload, {}};
		}
		const Publication publication{*this};
		const auto committed = editor_.commit();
		if (!committed)
			return {client::Status::CodecError, inspector::ModelStatus::InvalidPayload, {}};
		completion_.bind(
		    [fn = std::forward<F>(callback)](const Completion& result) mutable noexcept {
			    if constexpr (Service)
				    fn(result.service);
			    else
				    fn(result.command);
		    });
		const auto gateAfterMove = model_.admissionGate();
		const auto capturedGate = gateAfterMove == client::Status::Ok && !editor_.current()
		                              ? client::Status::Cancelled
		                              : gateAfterMove;
		if (capturedGate != client::Status::Ok) {
			completion_.reset();
			(void)editor_.rearm();
			return {capturedGate, inspector::ModelStatus::Ok, {}};
		}
		auto completed = model_.holdCallback([this](const dynamic::DynamicResult& result) noexcept {
			if constexpr (Service)
				serviceCompleted(result);
			else
				commandCompleted(result.operation);
		});
		const auto submitted = [&]() noexcept {
			if constexpr (Service)
				return model_.client().service(requestBinding_, committed.value,
				                               std::move(completed), timeout);
			else
				return model_.client().command(requestBinding_, committed.value,
				                               std::move(completed), timeout);
		}();
		const bool localRevoked = [&]() noexcept {
			if constexpr (Borrowed)
				return model_.admissionGate() != client::Status::Ok || !editor_.current();
			return false;
		}();
		(void)editor_.rearm();
		if (!submitted) {
			completion_.reset();
			return {submitted.status, inspector::ModelStatus::Ok, {}};
		}
		model_.snapshot_.retire();
		if constexpr (Service) {
			(void)responseStore_.selectEndpoint(requestBinding_.id(),
			                                    requestBinding_.responseType(),
			                                    descriptor::Category::Service);
			serviceRevision_ = responseStore_.revision();
		} else
			responseStore_.clearSelection();
		phase_ = Phase::Executing;
		if (localRevoked)
			(void)model_.client().cancel(submitted.handle);
		return {client::Status::Ok, inspector::ModelStatus::Ok, submitted.handle};
	}

	[[nodiscard]] bool editCurrent() const noexcept
	{
		return editor_.current() &&
		       (requestSelected_ ? model_.isCurrent(requestBinding_)
		                         : (model_.isCurrent(editRead_) && model_.isCurrent(editWrite_)));
	}

	[[nodiscard]] client::Status publicationStatus() const noexcept
	{
		const auto gate = model_.submissionGate();
		return gate == client::Status::Ok ? client::Status::Busy : gate;
	}

	void retireEditor() noexcept
	{
		editor_.clear();
		clearRequest();
		if (phase_ == Phase::Editing)
			phase_ = Phase::Idle;
	}

	void clearRequest() noexcept
	{
		if (requestSelected_) {
			editor_.clear();
			requestSelected_ = false;
			requestStore_.clearSelection();
		}
	}

	void commandCompleted(const client::ResultView& operation) noexcept
	{
		const inspector::OperationResult result{operation.status, operation.operation,
		                                        operation.dispatch, operation.endpointStatus,
		                                        operation.codec};
		finishRequest(Completion{{}, result, {}});
	}

	void serviceCompleted(const dynamic::DynamicResult& result) noexcept
	{
		const auto& operation = result.operation;
		ServiceResult owned{{operation.status, operation.operation, operation.dispatch,
		                     operation.endpointStatus, operation.codec},
		                    result.valueStatus,
		                    inspector::ModelStatus::Stale,
		                    requestBinding_.id(),
		                    serviceRevision_,
		                    false};
		if (Borrowed && result.valueStatus != dynamic::ValueStatus::NoValue &&
		    (!model_.isCurrent() || !responseStore_.isCurrent() ||
		     responseStore_.revision() != serviceRevision_)) {
			owned.valueStatus = dynamic::ValueStatus::Stale;
			finishRequest(Completion{{}, {}, owned});
			return;
		}
		if (result.valueStatus == dynamic::ValueStatus::NoValue)
			owned.modelStatus = inspector::ModelStatus::Ok;
		else if (result.valueStatus == dynamic::ValueStatus::TypeMismatch)
			owned.modelStatus = inspector::ModelStatus::TypeMismatch;
		else if (result.valueStatus == dynamic::ValueStatus::InvalidPayload)
			owned.modelStatus = inspector::ModelStatus::InvalidPayload;
		else if (result.hasValue()) {
			owned.modelStatus = responseStore_.publishValidated(
			    operation.payload, responseStore_.admission(), serviceRevision_);
			owned.published = owned.modelStatus == inspector::ModelStatus::Ok;
		}
		finishRequest(Completion{{}, {}, owned});
	}

	void finishRequest(const Completion& result) noexcept
	{
		if (requestCurrent() && editor_.current() && model_.submissionGate() == client::Status::Ok)
			phase_ = Phase::Editing;
		else {
			clearRequest();
			phase_ = Phase::Idle;
		}
		completionActive_ = true;
		completion_.invoke(result);
		{
			const Publication publication{*this};
			completion_.reset();
		}
		completionActive_ = false;
	}

	void finishApply() noexcept
	{
		phase_ = Phase::Idle;
		const auto result = applyResult_;
		completionActive_ = true;
		completion_.invoke(Completion{result, {}});
		{
			const Publication publication{*this};
			completion_.reset();
		}
		completionActive_ = false;
	}

	void writeCompleted(const dynamic::DynamicResult& result, std::uint32_t refreshTimeout) noexcept
	{
		const auto& operation = result.operation;
		applyResult_.write = {operation.status, operation.operation, operation.dispatch,
		                      operation.endpointStatus, operation.codec};
		applyResult_.writeCompleted = true;
		if (!operation.succeeded()) {
			finishApply();
			return;
		}
		applyResult_.refreshAttempted = true;
		if (!model_.isCurrent(editRead_)) {
			applyResult_.refreshSubmission = model_.submissionGate();
			if (applyResult_.refreshSubmission == client::Status::Ok)
				applyResult_.refreshSubmission = client::Status::NotAdmitted;
			finishApply();
			return;
		}
		if (model_.snapshot_.admission() != applyAdmission_ ||
		    model_.snapshot_.revision() != applyRevision_) {
			applyResult_.refreshSubmission = client::Status::Cancelled;
			finishApply();
			return;
		}
		phase_ = Phase::Refreshing;
		const auto submitted = model_.readField(
		    editRead_,
		    [this](const inspector::ReadResult& read) noexcept {
			    applyResult_.refresh = read;
			    applyResult_.refreshCompleted = true;
			    finishApply();
		    },
		    refreshTimeout);
		applyResult_.refreshSubmission = submitted.status;
		applyResult_.refreshSubmitted = bool(submitted);
		if (!submitted)
			finishApply();
	}
};

} // namespace detail

// Existing owning API. It alone admits sessions and drives the shared transport.
template<std::size_t N = 8, std::size_t PayloadBytes = 4096, std::size_t SnapshotBytes = 4096,
         std::size_t CallbackBytes = 256>
class WorkbenchModel
    : public detail::WorkbenchState<N, PayloadBytes, SnapshotBytes, CallbackBytes, false> {
	using Base = detail::WorkbenchState<N, PayloadBytes, SnapshotBytes, CallbackBytes, false>;

public:
	using Base::Base;
};

// Independent local presentation state over an existing owner's one client/FIFO.
// The owner and schema backing storage outlive this object. Before destruction,
// retire and wait for drained(): callbacks and their captured destructors must
// return. This local proof says nothing about physical or remote quiescence.
template<std::size_t N = 8, std::size_t PayloadBytes = 4096, std::size_t SnapshotBytes = 4096,
         std::size_t CallbackBytes = 256>
class WorkbenchView
    : private detail::WorkbenchState<N, PayloadBytes, SnapshotBytes, CallbackBytes, true> {
	using Base = detail::WorkbenchState<N, PayloadBytes, SnapshotBytes, CallbackBytes, true>;

public:
	explicit WorkbenchView(WorkbenchModel<N, PayloadBytes, SnapshotBytes, CallbackBytes>& owner,
	                       std::span<std::byte> responseStorage = {}) noexcept
	    : Base(owner, responseStorage)
	{}

	using Base::catalogs;
	using Base::endpoints;
	using Base::fields;
	using Base::commands;
	using Base::services;
	using Base::bindField;
	using Base::selectField;
	using Base::clearSelection;
	using Base::readField;
	using Base::refresh;
	using Base::snapshotInfo;
	using Base::root;
	using Base::info;
	using Base::childCount;
	using Base::child;
	using Base::scalar;
	using Base::decimal;
	using Base::isCurrent;
	using Base::phase;
	using Base::editing;
	using Base::canEdit;
	using Base::dirty;
	using Base::canApply;
	using Base::copyDraft;
	using Base::beginEdit;
	using Base::setScalar;
	using Base::editScalar;
	using Base::cancelEdit;
	using Base::apply;
	using Base::bindCommand;
	using Base::selectCommand;
	using Base::bindService;
	using Base::selectService;
	using Base::commandSelected;
	using Base::serviceSelected;
	using Base::canExecute;
	using Base::canCallService;
	using Base::editorSnapshotInfo;
	using Base::editorRoot;
	using Base::editorInfo;
	using Base::editorChild;
	using Base::editorChildCount;
	using Base::execute;
	using Base::callService;
	using Base::serviceSnapshotInfo;
	using Base::serviceRoot;
	using Base::serviceInfo;
	using Base::serviceChild;
	using Base::serviceChildCount;
	using Base::serviceScalar;
	using Base::serviceDecimal;
	using Base::session;
	using Base::drained;
	using Base::synchronize;
	using Base::retire;
	using Base::invalidateData;
};

} // namespace telemetry::integration::workbench
#endif // TELEMETRY_INTEGRATION_WORKBENCH_WORKBENCH_MODEL_HPP

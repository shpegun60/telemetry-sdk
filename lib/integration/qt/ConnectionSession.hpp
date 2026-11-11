/*
 * @file ConnectionSession.hpp
 * @brief Optional Qt owner for the named Resource/TCP bootstrap profile.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_QT_CONNECTION_SESSION_HPP
#define TELEMETRY_INTEGRATION_QT_CONNECTION_SESSION_HPP
#pragma once

#include <integration/qt/ConnectionStorage.hpp>
#include <integration/read_many/Workbench.hpp>
#include <integration/batch/Workbench.hpp>
#include <integration/files/Workbench.hpp>
#include <QtCore/QStringList>
#include <functional>
#include <optional>

namespace telemetry::integration::qt {
// Host-only convenience owner. TCP here means the documented, exclusive
// Resource bootstrap + Encoded stream reference profile, not an arbitrary TCP
// server inferred from its URL. Reconnect is explicit and never replays I/O.
class ConnectionSession final : public QObject {
	Q_OBJECT
	Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
	Q_PROPERTY(bool connecting READ connecting NOTIFY stateChanged)
	Q_PROPERTY(bool recoveryRequired READ recoveryRequired NOTIFY stateChanged)
	Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
	Q_PROPERTY(bool readManySupported READ readManySupported NOTIFY admissionChanged)
	Q_PROPERTY(bool batchSupported READ batchSupported NOTIFY admissionChanged)
	Q_PROPERTY(bool busy READ busy NOTIFY progressed)
	Q_PROPERTY(bool filesOnly READ filesOnly NOTIFY stateChanged)
	Q_PROPERTY(Limitation limitation READ limitation NOTIFY stateChanged)
	Q_PROPERTY(QStringList discoveredPaths READ discoveredPaths NOTIFY stateChanged)
	Q_PROPERTY(quint64 descriptorReceived READ descriptorReceived NOTIFY discoveryProgressed)
	Q_PROPERTY(quint64 descriptorTotal READ descriptorTotal NOTIFY discoveryProgressed)
public:
	// Why a connection carries the device's files only. The program words it;
	// lastError() holds an English description of the same.
	enum class Limitation {
		None,
		// The device's file list has no /telemetry/descriptor.bin.
		NoDescriptor,
		// The device refused to stat or read the descriptor.
		DescriptorUnreadable,
		// The descriptor's size is outside what a client accepts.
		DescriptorSize,
		// The descriptor was read but could not be parsed or admitted.
		DescriptorRejected,
		// A ReadMany, Batch or session file is missing, malformed or does not match.
		Capability
	};
	Q_ENUM(Limitation)

	using StorageFactory = std::function<std::shared_ptr<ConnectionStorage>(QByteArray)>;
	explicit ConnectionSession(QObject* parent = nullptr);
	// Advanced seam for an existing host's outcome slots. The factory supplies
	// storage only; this session remains the sole admission/progress owner.
	explicit ConnectionSession(StorageFactory factory, QObject* parent = nullptr);
	~ConnectionSession() override;
	Q_INVOKABLE bool connectTcpReference(const QString& host, quint16 port);
	Q_INVOKABLE void disconnect();
	Q_INVOKABLE void close();
	bool ready() const;
	bool connecting() const;
	bool recoveryRequired() const;
	QString lastError() const;
	bool busy() const noexcept;
	bool readManySupported() const noexcept;
	std::optional<read_many::wire::Capability> readManyCapability() const noexcept;
	bool batchSupported() const noexcept;
	std::optional<batch::wire::Capability> batchCapability() const noexcept;
	// Whether the device's files can be read on this connection: it is ready,
	// or connected files only, and its transport frames Resource replies.
	bool filesSupported() const noexcept;
	// Connected to a device that lists its files correctly but offers no usable
	// descriptor: telemetry is unavailable (ready() is false), its files are not.
	bool filesOnly() const noexcept;
	Limitation limitation() const noexcept;
	// The paths the device listed while connecting, in its order; empty when
	// discovery did not get that far. For naming a mistyped descriptor.
	QStringList discoveredPaths() const;
	// The descriptor's size when DescriptorSize limited the connection.
	quint64 refusedDescriptorSize() const noexcept;
	// Reading the descriptor while connecting, for a progress bar: bytes so far
	// of its size; both zero until the device has said how big it is.
	quint64 descriptorReceived() const noexcept;
	quint64 descriptorTotal() const noexcept;

	// Cancels a request of this session's queue that has not been sent yet; its
	// callback then reports Cancelled. False when there is nothing to cancel.
	bool cancel(client::RequestHandle handle) noexcept;

	// A raw Resource List, Stat or Read on this session's one queue. This makes
	// the session a files client: files::list, stat, read and FileReader take
	// it directly. The reply view ends at callback return.
	template<class F>
	client::Submission submitResource(client::Input request, std::uint32_t replyBytes, F&& callback,
	                                  std::uint32_t timeoutMs = 30000) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const client::ResultView&>);
		static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>);
		client::Status status;
		if (const auto files = filesOnlyStorage(status)) {
			if (!filesSupported())
				return {client::Status::Unsupported, {}};
			return files->client.submitResource(
			    request, replyBytes,
			    [files, callable = std::forward<F>(callback)](
			        const client::ResultView& result) mutable noexcept {
				    callable(result);
			    },
			    timeoutMs);
		}
		if (status != client::Status::Ok)
			return {status, {}};
		auto owner = operationStorage(status);
		if (!owner)
			return {status, {}};
		if (!filesSupported())
			return {client::Status::Unsupported, {}};
		return files::Submitter<HostWorkbench>::submit(
		    owner->inspector, request, replyBytes,
		    [owner, callable = std::forward<F>(callback)](
		        const client::ResultView& result) mutable noexcept {
			    callable(result);
		    },
		    timeoutMs);
	}

	struct BatchRequest {
		client::Operation operation;
		std::uint32_t id;
		client::Input payload;
	};

	// One bounded request through the existing FIFO. Explicit invocation is
	// authorization; GUI hosts apply their shared mutation permission first.
	// Request spans end at return; result views end at callback return.
	template<class F>
	client::Submission batch(std::span<const BatchRequest> requests, F&& callback,
	                         std::uint32_t timeoutMs = 30000) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const batch::BatchResult&>);
		static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>);
		client::Status status;
		auto owner = operationStorage(status);
		if (!owner)
			return {status, {}};
		if (!owner->batch)
			return {client::Status::Unsupported, {}};
		if (requests.empty() || requests.size() > batch::wire::maxItems || !timeoutMs ||
		    timeoutMs > client::maxTimeoutMs)
			return {client::Status::InvalidArgument, {}};
		if constexpr (std::is_pointer_v<std::decay_t<F>>)
			if (!callback)
				return {client::Status::InvalidArgument, {}};
		SubmissionScope scope{*this};
		std::array<batch::Endpoint, batch::wire::maxItems> endpoints{};
		std::array<client::Input, batch::wire::maxItems> payloads{};
		bool mutation = false;
		for (std::size_t i = 0; i < requests.size(); ++i) {
			endpoints[i] = {requests[i].operation, requests[i].id};
			payloads[i] = requests[i].payload;
			mutation = mutation || requests[i].operation != client::Operation::FieldRead;
		}
		const auto binding = batch::bindBatch(owner->inspector, owner->backend, *owner->batch,
		                                      std::span{endpoints}.first(requests.size()));
		if (!binding)
			return {binding.status, {}};
		const auto submitted = batch::batch(
		    owner->inspector, binding.binding, std::span{payloads}.first(requests.size()),
		    owner->batchScratch,
		    [owner, callable = std::forward<F>(callback)](
		        const batch::BatchResult& result) mutable noexcept {
			    callable(result);
		    },
		    timeoutMs);
		scope.mutation = bool(submitted) && mutation;
		return submitted;
	}

	// Snapshot-preserving operations over the same Workbench/FIFO. Result views
	// borrow storage only during callback; accepted callbacks retain that storage
	// through Session retirement. These guards never advance process().
	template<class F>
	client::Submission readMany(std::span<const std::uint32_t> ids, F&& callback,
	                            std::uint32_t timeoutMs = 30000) noexcept
	{
		static_assert(
		    std::is_nothrow_invocable_v<std::decay_t<F>&, const read_many::ReadManyResult&>);
		static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>);
		client::Status status;
		auto owner = operationStorage(status);
		if (!owner)
			return {status, {}};
		if (!owner->readMany)
			return {client::Status::Unsupported, {}};
		if (!timeoutMs || timeoutMs > client::maxTimeoutMs)
			return {client::Status::InvalidArgument, {}};
		if constexpr (std::is_pointer_v<std::decay_t<F>>)
			if (!callback)
				return {client::Status::InvalidArgument, {}};
		const auto binding =
		    read_many::bindReadMany(owner->inspector, owner->backend, *owner->readMany, ids);
		if (!binding)
			return {binding.status, {}};
		return read_many::readMany(
		    owner->inspector, binding.binding,
		    [owner, callable = std::forward<F>(callback)](
		        const read_many::ReadManyResult& result) mutable noexcept {
			    callable(result);
		    },
		    timeoutMs);
	}

	template<class F>
	client::Submission readFieldValue(std::uint32_t id, F&& callback,
	                                  std::uint32_t timeoutMs = 30000) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const dynamic::DynamicResult&>);
		static_assert(std::is_nothrow_constructible_v<std::decay_t<F>, F&&>);
		client::Status status;
		auto owner = operationStorage(status);
		if (!owner)
			return {status, {}};
		if (!timeoutMs || timeoutMs > client::maxTimeoutMs)
			return {client::Status::InvalidArgument, {}};
		if constexpr (std::is_pointer_v<std::decay_t<F>>)
			if (!callback)
				return {client::Status::InvalidArgument, {}};
		const auto binding = owner->inspector.bindField(id);
		if (!binding)
			return {binding.status, {}};
		return read_many::readFieldValue(
		    owner->inspector, binding.endpoint,
		    [owner, callable = std::forward<F>(callback)](
		        const dynamic::DynamicResult& result) mutable noexcept {
			    callable(result);
		    },
		    timeoutMs);
	}

	std::shared_ptr<ConnectionStorage> storage() const;
	HostWorkbench* workbench() const;
	// Useful when one host also admits other explicit transport profiles. It
	// cannot lower/reuse a session number and performs no native processing.
	bool reserveSessionsThrough(quint32 lastUsed);
	void setWriteChunkBytes(int bytes);
	void setExchangeTimeout(int milliseconds);
	// A GUI event-loop timer calls this automatically. Exposed for controlled
	// owner-loop/test integration; recursive calls do not advance native work.
	void process();
Q_SIGNALS:
	// Synchronous: detach presentation BEFORE closing/draining native work.
	void aboutToRetire();
	void admissionChanged();
	// Accepted mutating batch retired native drafts/snapshots before this signal.
	// Presentation siblings retire their owned caches, without changing admission.
	void dataInvalidated();
	// Only after the core callback stack has returned. Borrowing UI adapters
	// consume owned outcome slots here; no Qt publication occurs inside core.
	void progressed();
	void stateChanged();
	void discoveryProgressed();

private:
	std::shared_ptr<ConnectionStorage> operationStorage(client::Status& status) const noexcept;
	// The files-only storage when connected files only. Otherwise null, with
	// status Ok when the caller should try the telemetry storage, or the reason.
	std::shared_ptr<FilesOnlyStorage> filesOnlyStorage(client::Status& status) const noexcept;
	struct Impl;

	struct SubmissionScope {
		std::shared_ptr<Impl> state;
		bool mutation = false;
		explicit SubmissionScope(ConnectionSession&) noexcept;
		~SubmissionScope();
	};

	std::shared_ptr<Impl> d;
};
} // namespace telemetry::integration::qt
#endif

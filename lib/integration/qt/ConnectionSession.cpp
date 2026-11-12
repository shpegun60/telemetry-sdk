/*
 * @file ConnectionSession.cpp
 * @brief One Qt transport/session owner with explicit same-peer recovery.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include <integration/qt/ConnectionSession.hpp>
#include <integration/qt/TcpTransport.hpp>
#include <wire/IdentityWire.hpp>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <algorithm>
#include <limits>

namespace telemetry::integration::qt {
struct ConnectionSession::Impl {
	QPointer<ConnectionSession> owner;
	StorageFactory factory;
	QTimer timer;
	std::shared_ptr<ConnectionStorage> current, retired;
	std::shared_ptr<FilesOnlyStorage> filesOnly;
	std::unique_ptr<TcpTransport> transport;
	ConnectionSession::Limitation limitation = ConnectionSession::Limitation::None;
	QStringList discoveredPaths;
	QString limitationText;
	quint64 refusedDescriptorSize = 0, descriptorReceived = 0, descriptorTotal = 0;
	QString host, peerAddress, retiredHost, retiredAddress, error;
	quint16 port = 0, retiredPort = 0;
	quint32 lastSession = 0;
	quint64 generation = 0;
	int writeChunk = 0, exchangeTimeout = 10000;
	bool connected = false, opening = false, terminal = false, working = false;
	// One process() already queued for the owner loop's next turn.
	bool pumpQueued = false;
	bool retiring = false, closeRequested = false, nativeActive = false;

	// The device's identity file: Stat, then Read until its end, one request
	// at a time. Completions only record; progress() submits the next step,
	// since a queue refuses submissions from inside its own completions.
	struct IdentityRead {
		enum class Step : std::uint8_t {
			Idle,
			Stat,
			Read,
			Done
		};
		Step step = Step::Idle;
		quint64 epoch = 0; // the connection it belongs to
		::resource::FileIndex index = 0;
		::resource::Cursor cursor = 0;
		::resource::FileSize size = 0;
		unsigned idlePieces = 0;
		QByteArray bytes;
		bool due = false, inFlight = false, changed = false;
	} identityRead;

	ConnectionSession::IdentityState identityState = ConnectionSession::IdentityState::Absent;
	QString identityId, identityName, identityModel, identityFirmware, identityProblem;

	// While set, a WRITE to one of the SDK's own files is let through: only
	// the session's own requests (the rename) set it.
	bool protocolWriteAllowed = false;
	struct ProtocolWrite {
		Impl& state;

		explicit ProtocolWrite(Impl& owner) noexcept : state(owner)
		{
			state.protocolWriteAllowed = true;
		}
		~ProtocolWrite()
		{
			state.protocolWriteAllowed = false;
		}
		ProtocolWrite(const ProtocolWrite&) = delete;
		ProtocolWrite& operator=(const ProtocolWrite&) = delete;
	};

	// Renaming: one WRITE of the whole identity file, then the file read again.
	struct IdentityRename {
		enum class Step : std::uint8_t {
			Idle,
			Write,
			ReadBack,
			Ended
		};
		Step step = Step::Idle;
		quint64 epoch = 0;
		QByteArray bytes;
		QString name, problem;
		std::array<std::byte, ::resource::protocol::wire::writeRequestHeaderSize +
		                          ::telemetry::protocol::identity::writtenBytes>
		    scratch{};
		bool due = false, inFlight = false, renamed = false;
	} identityRename;

	Impl(ConnectionSession& target, StorageFactory value)
	    : owner(&target), factory(std::move(value))
	{
		timer.setInterval(5);
		QObject::connect(&timer, &QTimer::timeout, &target, [&target] {
			target.process();
		});
	}

	void notify()
	{
		if (owner)
			Q_EMIT owner->stateChanged();
	}

	void publishProgress()
	{
		if (owner)
			Q_EMIT owner->progressed();
	}

	void closeNative()
	{
		if (transport) {
			transport->close();
			if (!transport->locallyQuiescent())
				qFatal("ConnectionSession transport did not release local TX/RX storage");
		}
		if (current)
			current->closeAndDrain();
		if (filesOnly)
			filesOnly->closeAndDrain();
	}

	void retire()
	{
		if (retiring)
			return;
		retiring = true;
		timer.stop();
		++generation;
		connected = opening = false;
		if (current && owner)
			Q_EMIT owner->aboutToRetire();
		closeNative();
		if (current) {
			publishProgress();
			if (current->inspector.recoveryRequired()) {
				if (retired)
					qFatal("A previous TCP session still requires recovery");
				retired = current;
				retiredHost = host;
				retiredPort = port;
				retiredAddress = peerAddress;
			}
			current.reset();
		}
		// A files-only connection never needs same-peer recovery: it executed
		// nothing, so a new connection is all a later one needs.
		filesOnly.reset();
		// The limitation describes the live connection; the paths stay until the
		// next connection, for naming a typo after this one has gone.
		limitation = ConnectionSession::Limitation::None;
		limitationText.clear();
		transport.reset();
		// The identity is the connected device's; a completion still drained
		// carries the old epoch and is ignored.
		const bool hadIdentity = identityState != ConnectionSession::IdentityState::Absent;
		forgetIdentity();
		retiring = false;
		if (owner)
			Q_EMIT owner->admissionChanged();
		notify();
		if (hadIdentity && owner)
			Q_EMIT owner->identityChanged();
		// A rename the disconnect cut short ends now, not on a later connection.
		if (identityRename.step == IdentityRename::Step::Ended) {
			identityRename.step = IdentityRename::Step::Idle;
			const auto problem = identityRename.problem;
			identityRename.bytes.clear();
			if (owner)
				Q_EMIT owner->renameFinished(false, problem);
		}
	}

	void forgetIdentity()
	{
		// A rename of the identity forgotten ends with it.
		if (identityRename.step == IdentityRename::Step::Write ||
		    identityRename.step == IdentityRename::Step::ReadBack) {
			identityRename.step = IdentityRename::Step::Ended;
			identityRename.renamed = false;
			identityRename.problem = QStringLiteral("the connection ended before the device answered");
			identityRename.due = false;
			identityRename.inFlight = false;
		}
		identityRead = {};
		identityState = ConnectionSession::IdentityState::Absent;
		identityId.clear();
		identityName.clear();
		identityModel.clear();
		identityFirmware.clear();
		identityProblem.clear();
	}

	// Called as a connection becomes ready or files only, before it says so:
	// a listed identity file is then already Reading when a client looks.
	void beginIdentity()
	{
		forgetIdentity();
		identityRead.epoch = generation;
		identityRead.step = IdentityRead::Step::Done;
		const auto at = discoveredPaths.indexOf(QString::fromLatin1(
		    ::telemetry::protocol::identity::path.data(),
		    static_cast<qsizetype>(::telemetry::protocol::identity::path.size())));
		// discoveredPaths holds the device's list in its order, so a position
		// is the file's index.
		if (at < 0)
			return;
		identityRead.index = static_cast<::resource::FileIndex>(at);
		identityRead.step = IdentityRead::Step::Stat;
		identityRead.due = true;
		identityState = ConnectionSession::IdentityState::Reading;
		identityRead.changed = true;
	}

	void identityFailed(QString reason)
	{
		identityRead.step = IdentityRead::Step::Done;
		identityRead.due = false;
		identityRead.bytes.clear();
		identityState = ConnectionSession::IdentityState::Unreadable;
		identityProblem = std::move(reason);
		identityRead.changed = true;
		if (identityRename.step == IdentityRename::Step::ReadBack) {
			identityRename.step = IdentityRename::Step::Ended;
			identityRename.renamed = false;
			identityRename.problem = QStringLiteral("its identity file could not be read again: %1").arg(identityProblem);
		}
	}

	void identityStatted(quint64 epoch, const files::StatResult& result) noexcept
	{
		if (epoch != generation || identityRead.epoch != epoch ||
		    identityRead.step != IdentityRead::Step::Stat)
			return;
		identityRead.inFlight = false;
		if (!result.succeeded())
			return identityFailed(QStringLiteral("the device did not answer for it"));
		if (!::resource::has(result.reply.flags, ::resource::FileFlag::Readable))
			return identityFailed(QStringLiteral("the device does not let it be read"));
		if (result.reply.size > ::telemetry::protocol::identity::maxBytes)
			return identityFailed(
			    QStringLiteral("it is %1 bytes, more than an identity has").arg(result.reply.size));
		identityRead.size = result.reply.size;
		identityRead.cursor = 0;
		identityRead.step = IdentityRead::Step::Read;
		identityRead.due = true;
	}

	void identityPiece(quint64 epoch, const files::ReadResult& result) noexcept
	{
		if (epoch != generation || identityRead.epoch != epoch ||
		    identityRead.step != IdentityRead::Step::Read)
			return;
		identityRead.inFlight = false;
		if (!result.succeeded())
			return identityFailed(QStringLiteral("the device did not let it be read"));
		const auto& piece = result.reply.data;
		if (static_cast<std::size_t>(identityRead.bytes.size()) + piece.size() >
		    ::telemetry::protocol::identity::maxBytes)
			return identityFailed(QStringLiteral("it is longer than an identity can be"));
		if (!piece.empty())
			identityRead.bytes.append(reinterpret_cast<const char*>(piece.data()),
			                          static_cast<qsizetype>(piece.size()));
		if (!result.reply.eof) {
			if (piece.empty() &&
			    (result.reply.next == identityRead.cursor || ++identityRead.idlePieces > 64))
				return identityFailed(QStringLiteral("the device stopped sending it"));
			identityRead.cursor = result.reply.next;
			identityRead.due = true;
			return;
		}
		identityRead.step = IdentityRead::Step::Done;
		telemetry::protocol::identity::Identity parsed;
		const auto problem = ::telemetry::protocol::identity::parse(
		    {reinterpret_cast<const std::byte*>(identityRead.bytes.constData()),
		     static_cast<std::size_t>(identityRead.bytes.size())},
		    parsed);
		if (problem != ::telemetry::protocol::identity::Problem::None) {
			const auto words = ::telemetry::protocol::identity::describe(problem);
			return identityFailed(
			    QString::fromLatin1(words.data(), static_cast<qsizetype>(words.size())));
		}
		const auto text = [](std::string_view value) {
			return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
		};
		identityId = text(parsed.id);
		identityName = text(parsed.name);
		identityModel = text(parsed.model);
		identityFirmware = text(parsed.firmware);
		identityState = ConnectionSession::IdentityState::Known;
		identityRead.bytes.clear();
		identityRead.changed = true;
		// Read back after a rename: the name the device now serves decides.
		if (identityRename.step == IdentityRename::Step::ReadBack) {
			identityRename.step = IdentityRename::Step::Ended;
			identityRename.renamed = identityName == identityRename.name;
			if (!identityRename.renamed)
				identityRename.problem = QStringLiteral("the device kept the name \"%1\"").arg(identityName);
		}
	}

	// The WRITE of the renamed file answered.
	void renameWritten(quint64 epoch, const files::WriteResult& result) noexcept
	{
		if (epoch != generation || identityRename.epoch != epoch ||
		    identityRename.step != IdentityRename::Step::Write)
			return;
		identityRename.inFlight = false;
		if (result.succeeded() && result.reply.complete &&
		    result.reply.consumed == static_cast<std::uint32_t>(identityRename.bytes.size())) {
			// Read it again: the device decides what it serves.
			identityRename.step = IdentityRename::Step::ReadBack;
			identityRead.step = IdentityRead::Step::Stat;
			identityRead.cursor = 0;
			identityRead.size = 0;
			identityRead.idlePieces = 0;
			identityRead.bytes.clear();
			identityRead.due = true;
			return;
		}
		identityRename.step = IdentityRename::Step::Ended;
		identityRename.renamed = false;
		if (result.operation.status != client::Status::Ok)
			identityRename.problem = QStringLiteral("the device did not answer; whether it took the name is not known");
		else if (!result.parsed)
			identityRename.problem = QStringLiteral("the device's answer was not one");
		else if (result.reply.status == ::resource::Status::Ok)
			identityRename.problem = QStringLiteral("the device did not finish taking the name");
		else if (result.reply.status == ::resource::Status::NotWritable)
			identityRename.problem = QStringLiteral("the device does not let its name be changed");
		else if (result.reply.status == ::resource::Status::InvalidData)
			identityRename.problem = QStringLiteral("the device refused the name");
		else
			identityRename.problem = QStringLiteral("the device refused it (status %1)")
			                             .arg(static_cast<int>(result.reply.status));
	}

	// Submits the identity's next request when one is due, and says when it
	// changed; from the owner loop only, outside every completion.
	void advanceIdentity()
	{
		if (!owner || working || retiring || !connected)
			return;
		auto& read = identityRead;
		if (read.due && !read.inFlight && read.epoch == generation) {
			struct Step {
				std::weak_ptr<Impl> state;
				quint64 epoch;

				void operator()(const files::StatResult& result) noexcept
				{
					if (const auto value = state.lock())
						value->identityStatted(epoch, result);
				}

				void operator()(const files::ReadResult& result) noexcept
				{
					if (const auto value = state.lock())
						value->identityPiece(epoch, result);
				}
			};

			const Step step{owner->d, read.epoch};
			const auto submitted = read.step == IdentityRead::Step::Stat
			                           ? files::stat(*owner, read.index, step)
			                           : files::read(*owner, read.index, read.cursor, step);
			if (submitted) {
				read.due = false;
				read.inFlight = true;
			} else if (submitted.status != client::Status::Busy &&
			           submitted.status != client::Status::QueueFull) {
				identityFailed(QStringLiteral("the connection could not ask for it"));
			}
		}
		// The rename's WRITE, never while the identity is being read.
		auto& rename = identityRename;
		if (rename.step == IdentityRename::Step::Write && rename.due && !rename.inFlight && !read.inFlight &&
		    rename.epoch == generation) {
			struct Written {
				std::weak_ptr<Impl> state;
				quint64 epoch;

				void operator()(const files::WriteResult& result) noexcept
				{
					if (const auto value = state.lock())
						value->renameWritten(epoch, result);
				}
			};

			const ProtocolWrite allowed{*this};
			const auto submitted = files::write(
			    *owner, ::resource::Output{rename.scratch.data(), rename.scratch.size()}, read.index, 0,
			    client::Input{reinterpret_cast<const std::byte*>(rename.bytes.constData()),
			                  static_cast<std::size_t>(rename.bytes.size())},
			    true, Written{owner->d, rename.epoch});
			if (submitted) {
				rename.due = false;
				rename.inFlight = true;
			} else if (submitted.status != client::Status::Busy &&
			           submitted.status != client::Status::QueueFull) {
				rename.step = IdentityRename::Step::Ended;
				rename.renamed = false;
				rename.problem = QStringLiteral("the connection could not send it");
			}
		}
		if (read.changed && owner) {
			read.changed = false;
			Q_EMIT owner->identityChanged();
		}
		if (rename.step == IdentityRename::Step::Ended && owner) {
			rename.step = IdentityRename::Step::Idle;
			const bool renamed = rename.renamed;
			const auto problem = rename.problem;
			rename.bytes.clear();
			Q_EMIT owner->renameFinished(renamed, problem);
		}
	}

	void fail(QString reason)
	{
		error = std::move(reason);
		retire();
	}

	static QString describe(ConnectionSession::Limitation limitation, quint64 size)
	{
		using L = ConnectionSession::Limitation;
		switch (limitation) {
			case L::NoDescriptor:
				return QStringLiteral(
				    "Not a Telemetry device: its file list has no /telemetry/descriptor.bin");
			case L::DescriptorUnreadable:
				return QStringLiteral(
				    "Telemetry is not available: the device refused to read /telemetry/descriptor.bin");
			case L::DescriptorSize:
				return QStringLiteral(
				           "Telemetry is not available: /telemetry/descriptor.bin is %1 bytes")
				    .arg(size);
			case L::DescriptorRejected:
				return QStringLiteral("Telemetry is not available: the descriptor was rejected");
			case L::Capability:
				return QStringLiteral(
				    "Telemetry is not available: a ReadMany, Batch or session file does not match");
			case L::None:
				break;
		}
		return {};
	}

	// Telemetry is impossible but the files were listed correctly: stay
	// connected for them. detail, when given, replaces the generic wording.
	void enterFilesOnly(ConnectionSession::Limitation reason, QString detail = {})
	{
		auto storage = std::make_shared<FilesOnlyStorage>();
		if (!storage->admit()) {
			fail(QStringLiteral("The files-only client could not be admitted"));
			return;
		}
		// Both ways in leave the transport carrying Resource requests only.
		if (transport)
			transport->restrictToFiles();
		limitation = reason;
		refusedDescriptorSize = transport ? transport->refusedDescriptorSize() : 0;
		limitationText =
		    detail.isEmpty() ? describe(reason, refusedDescriptorSize) : std::move(detail);
		// The offered session number may already be written to the device's
		// session file; never offer it again.
		if (lastSession != std::numeric_limits<quint32>::max())
			++lastSession;
		filesOnly = std::move(storage);
		connected = true;
		opening = false;
		// lastError() keeps meaning why a connection failed or ended.
		error.clear();
		beginIdentity();
		timer.start();
		if (owner)
			Q_EMIT owner->admissionChanged();
		notify();
	}

	void filesOnlyDiscovered(quint64 epoch)
	{
		if (epoch != generation || !opening || !transport || !transport->filesOnly())
			return;
		if (working) {
			// The transport has stopped its discovery deadline; nothing else
			// would end this attempt, so come back once the owner is free.
			if (!owner)
				return;
			QTimer::singleShot(0, owner, [target = owner.data(), epoch] {
				target->d->filesOnlyDiscovered(epoch);
			});
			return;
		}
		using R = DescriptorBootstrap::Refusal;
		using L = ConnectionSession::Limitation;
		discoveredPaths = transport->discoveredPaths();
		switch (transport->discoveryRefusal()) {
			case R::NoDescriptor:
				enterFilesOnly(L::NoDescriptor);
				return;
			case R::DescriptorUnreadable:
				enterFilesOnly(L::DescriptorUnreadable);
				return;
			case R::DescriptorSize:
				enterFilesOnly(L::DescriptorSize);
				return;
			case R::DescriptorLength:
				enterFilesOnly(
				    L::DescriptorRejected,
				    QStringLiteral("Telemetry is not available: /telemetry/descriptor.bin "
				                   "did not have the length the device listed"));
				return;
			case R::Capability:
				enterFilesOnly(L::Capability);
				return;
			case R::None:
			case R::Protocol:
				break;
		}
		fail(QStringLiteral("Invalid Descriptor discovery reply"));
	}

	void descriptor(quint64 epoch, QByteArray bytes)
	{
		if (epoch != generation || !opening || !transport || !transport->ready() || working)
			return;
		working = true;
		discoveredPaths = transport->discoveredPaths();
		std::shared_ptr<ConnectionStorage> candidate;
		QString failure;
		// A failure of the device's own files leaves its files readable.
		auto limited = ConnectionSession::Limitation::None;
		try {
			candidate = factory(std::move(bytes));
		} catch (const std::exception& exception) {
			failure = QStringLiteral("Connection storage preparation failed: %1")
			              .arg(QString::fromUtf8(exception.what()));
		} catch (...) {
			failure = QStringLiteral("Connection storage preparation failed");
		}
		// A user factory can reenter or destroy its QObject owner. Never touch
		// the transport/candidate admission after that ownership boundary.
		if (!owner || closeRequested || !transport || epoch != generation) {
			working = false;
			retire();
			return;
		}
		if (failure.isEmpty() && !candidate)
			failure = QStringLiteral("The connection storage factory returned no storage");
		else if (failure.isEmpty()) {
			failure = candidate->initialize(lastSession + 1, false);
			if (!failure.isEmpty())
				limited = ConnectionSession::Limitation::DescriptorRejected;
		}
		if (failure.isEmpty()) {
			const auto bytes = transport->readManyCapability();
			if (!bytes.isEmpty()) {
				const auto parsed = read_many::wire::parseCapability(
				    {reinterpret_cast<const std::byte*>(bytes.constData()),
				     static_cast<std::size_t>(bytes.size())});
				if (!parsed || parsed.value.session != lastSession + 1 ||
				    parsed.value.fingerprint != candidate->schema.fingerprint()) {
					failure = QStringLiteral(
					    "ReadMany capability does not match the requested session and Descriptor");
					limited = ConnectionSession::Limitation::Capability;
				} else
					candidate->readMany = parsed.value;
			}
		}
		if (failure.isEmpty()) {
			const auto bytes = transport->batchCapability();
			if (!bytes.isEmpty()) {
				const auto parsed = batch::wire::parseCapability(
				    {reinterpret_cast<const std::byte*>(bytes.constData()),
				     static_cast<std::size_t>(bytes.size())});
				if (!parsed || parsed.value.session != lastSession + 1 ||
				    parsed.value.fingerprint != candidate->schema.fingerprint()) {
					failure = QStringLiteral(
					    "Batch capability does not match the requested session and Descriptor");
					limited = ConnectionSession::Limitation::Capability;
				} else
					candidate->batch = parsed.value;
			}
		}
		if (failure.isEmpty() && retired) {
			// This named server profile serializes each client and completes old
			// native work before allowing the next full Resource transcript. TCP
			// closure alone never supplies this remote supersession proof.
			if (host != retiredHost || port != retiredPort ||
			    transport->peerAddress() != retiredAddress)
				failure = QStringLiteral("Reconnect the previous peer to finish session recovery");
			else if (!retired->inspector.confirmQuiescence(
			             {retired->inspector.session(), true, true, true}))
				failure = QStringLiteral("Previous session recovery could not be confirmed");
			else {
				retired.reset();
				retiredHost.clear();
				retiredAddress.clear();
				retiredPort = 0;
			}
		}
		if (failure.isEmpty())
			failure = candidate->admitSession(lastSession + 1);
		if (!failure.isEmpty() && limited != ConnectionSession::Limitation::None && owner &&
		    !closeRequested && transport->supportsResourceReplies()) {
			candidate.reset();
			working = false;
			enterFilesOnly(limited, QStringLiteral("Telemetry is not available: ") + failure);
			return;
		}
		if (!failure.isEmpty() || !owner || closeRequested) {
			working = false;
			fail(failure.isEmpty() ? QStringLiteral("Connection closed during admission")
			                       : std::move(failure));
			return;
		}
		++lastSession;
		peerAddress = transport->peerAddress();
		current = std::move(candidate);
		connected = true;
		opening = false;
		error.clear();
		beginIdentity();
		timer.start();
		if (owner)
			Q_EMIT owner->admissionChanged();
		notify();
		working = false;
		if (closeRequested || !owner)
			retire();
	}

	void progress()
	{
		if (!working && !retiring && connected && filesOnly && transport) {
			working = nativeActive = true;
			const auto files = filesOnly;
			files->progressTransport(transport.get());
			files->settleReplies();
			nativeActive = false;
			publishProgress();
			working = false;
			if (closeRequested || !owner)
				retire();
			else
				advanceIdentity();
			return;
		}
		if (working || retiring || !connected || !current || !transport)
			return;
		working = true;
		const auto state = current;
		nativeActive = true;
		(void)state->progressTransport(transport.get());
		state->settleReplies();
		nativeActive = false;
		publishProgress();
		const bool lost = !state->inspector.isCurrent() || state->inspector.recoveryRequired();
		working = false;
		if (closeRequested || !owner)
			retire();
		else if (lost)
			fail(QStringLiteral("Session is no longer current; reconnect explicitly"));
		else
			advanceIdentity();
	}
};

ConnectionSession::ConnectionSession(QObject* parent)
    : ConnectionSession(
          [](QByteArray bytes) {
	          return std::make_shared<ConnectionStorage>(std::move(bytes));
          },
          parent)
{}

ConnectionSession::SubmissionScope::SubmissionScope(ConnectionSession& owner) noexcept
    : state(owner.d)
{
	state->working = state->nativeActive = true;
}

ConnectionSession::SubmissionScope::~SubmissionScope()
{
	state->nativeActive = false;
	if (mutation && state->owner)
		Q_EMIT state->owner->dataInvalidated();
	state->working = false;
	if (state->closeRequested || !state->owner)
		state->retire();
	else
		state->notify();
}

ConnectionSession::ConnectionSession(StorageFactory factory, QObject* parent)
    : QObject(parent), d(std::make_shared<Impl>(*this, std::move(factory)))
{
	if (!d->factory)
		qFatal("ConnectionSession requires a storage factory");
}

ConnectionSession::~ConnectionSession()
{
	const auto state = d;
	state->owner.clear();
	state->terminal = state->closeRequested = true;
	state->timer.stop();
	if (!state->nativeActive) {
		// A model reset triggered by aboutToRetire may delete its Session.
		// Drain before QObject destroys the presentation registry children;
		// the original retirement stack retains Impl and finishes bookkeeping.
		if (state->retiring)
			state->closeNative();
		else
			state->retire();
	}
}

bool ConnectionSession::connectTcpReference(const QString& inputHost, quint16 inputPort)
{
	Q_ASSERT(thread() == QThread::currentThread());
	const auto state = d;
	if (state->terminal || state->working || state->retiring)
		return false;
	auto normalized = inputHost.trimmed().toLower();
	QHostAddress numeric;
	if (numeric.setAddress(normalized))
		normalized = numeric.toString().toLower();
	if (normalized.isEmpty() || !inputPort ||
	    state->lastSession == std::numeric_limits<quint32>::max()) {
		state->error = QStringLiteral("A host, nonzero port and unused session ID are required");
		state->notify();
		return false;
	}
	state->closeRequested = false;
	state->working = true;
	state->retire();
	if (!state->owner || state->terminal || state->closeRequested) {
		state->working = false;
		return false;
	}
	if (state->retired && (normalized != state->retiredHost || inputPort != state->retiredPort)) {
		state->working = false;
		state->error = QStringLiteral("Reconnect the previous peer to finish session recovery");
		state->notify();
		return false;
	}
	state->host = normalized;
	state->port = inputPort;
	state->error.clear();
	state->limitation = Limitation::None;
	state->discoveredPaths.clear();
	state->refusedDescriptorSize = 0;
	state->descriptorReceived = state->descriptorTotal = 0;
	state->opening = true;
	state->transport = std::make_unique<TcpTransport>();
	state->transport->setWriteChunkBytes(state->writeChunk);
	state->transport->setTimeout(state->exchangeTimeout);
	const auto epoch = state->generation;
	const std::weak_ptr<Impl> weak = state;
	QObject::connect(
	    state->transport.get(), &DeviceTransport::descriptorReceived, this,
	    [weak, epoch](QByteArray bytes) {
		    if (const auto value = weak.lock())
			    value->descriptor(epoch, std::move(bytes));
	    },
	    Qt::QueuedConnection);
	QObject::connect(
	    state->transport.get(), &DeviceTransport::packetAccepted, this,
	    [weak, epoch](quint32 session, quint32 id) {
		    if (const auto value = weak.lock(); value && value->generation == epoch) {
			    if (value->current)
				    (void)value->current->acceptPacket(session, id);
			    else if (value->filesOnly)
				    (void)value->filesOnly->acceptPacket(session, id);
		    }
	    },
	    Qt::QueuedConnection);
	QObject::connect(
	    state->transport.get(), &DeviceTransport::packetReceived, this,
	    [this, weak, epoch](const QByteArray& bytes, quint32 session, quint32 id) {
		    if (const auto value = weak.lock(); value && value->generation == epoch) {
			    if (value->current)
				    (void)value->current->receivePacket(bytes, session, id);
			    else if (value->filesOnly)
				    (void)value->filesOnly->receivePacket(bytes, session, id);
			    // Settle it, and send what waits behind it, without a tick's wait.
			    pumpSoon();
		    }
	    },
	    Qt::QueuedConnection);
	QObject::connect(
	    state->transport.get(), &DeviceTransport::transportError, this,
	    [weak, epoch](QString error) {
		    if (const auto value = weak.lock(); value && value->generation == epoch) {
			    // Complete replies are owned before EOF is reported. Never
			    // authorize another TX while settling the error.
			    value->working = value->nativeActive = true;
			    if (value->current)
				    value->current->settleReplies();
			    if (value->filesOnly)
				    value->filesOnly->settleReplies();
			    value->nativeActive = false;
			    // Publish the already-correlated result while its snapshot
			    // is current. Retirement still follows immediately and no
			    // new physical transmission is authorized in between.
			    value->publishProgress();
			    value->working = false;
			    value->fail(std::move(error));
		    }
	    },
	    Qt::QueuedConnection);
	QObject::connect(
	    state->transport.get(), &LinkTransport::discoveryProgressed, this,
	    [weak, epoch](quint64 received, quint64 total) {
		    if (const auto value = weak.lock(); value && value->generation == epoch) {
			    value->descriptorReceived = received;
			    value->descriptorTotal = total;
			    if (value->owner)
				    Q_EMIT value->owner->discoveryProgressed();
		    }
	    },
	    Qt::QueuedConnection);
	QObject::connect(
	    state->transport.get(), &LinkTransport::filesOnlyReached, this,
	    [weak, epoch] {
		    if (const auto value = weak.lock())
			    value->filesOnlyDiscovered(epoch);
	    },
	    Qt::QueuedConnection);
	state->transport->connectToWithEncoded(normalized, inputPort, state->lastSession + 1);
	state->working = false;
	state->notify();
	return true;
}

void ConnectionSession::disconnect()
{
	const auto state = d;
	if (state->working || state->retiring) {
		state->closeRequested = true;
		return;
	}
	state->retire();
}

void ConnectionSession::close()
{
	const auto state = d;
	state->terminal = true;
	disconnect();
}

bool ConnectionSession::ready() const
{
	return d->connected && d->current && d->current->inspector.isCurrent();
}

bool ConnectionSession::connecting() const
{
	return d->opening;
}

bool ConnectionSession::recoveryRequired() const
{
	return bool(d->retired) || (d->current && d->current->inspector.recoveryRequired());
}

QString ConnectionSession::lastError() const
{
	return d->error;
}

bool ConnectionSession::busy() const noexcept
{
	return d->current && d->current->inspector.pending() != 0;
}

bool ConnectionSession::readManySupported() const noexcept
{
	return ready() && bool(d->current->readMany);
}

std::optional<read_many::wire::Capability> ConnectionSession::readManyCapability() const noexcept
{
	return readManySupported() ? d->current->readMany : std::nullopt;
}

bool ConnectionSession::batchSupported() const noexcept
{
	return ready() && bool(d->current->batch);
}

std::optional<batch::wire::Capability> ConnectionSession::batchCapability() const noexcept
{
	return batchSupported() ? d->current->batch : std::nullopt;
}

bool ConnectionSession::cancel(client::RequestHandle handle) noexcept
{
	if (const auto files = d->filesOnly)
		return files->client.cancel(handle);
	const auto owner = d->current;
	return owner && files::Submitter<HostWorkbench>::cancel(owner->inspector, handle);
}

bool ConnectionSession::filesSupported() const noexcept
{
	if (filesOnly())
		return d->transport->supportsResourceReplies();
	return ready() && d->transport && d->transport->supportsResourceReplies() &&
	       d->current->backend.supportsResource();
}

bool ConnectionSession::filesOnly() const noexcept
{
	return d->connected && d->filesOnly && d->transport;
}

ConnectionSession::Limitation ConnectionSession::limitation() const noexcept
{
	return d->limitation;
}

QString ConnectionSession::limitationText() const
{
	return d->limitationText;
}

QStringList ConnectionSession::discoveredPaths() const
{
	return d->discoveredPaths;
}

quint64 ConnectionSession::refusedDescriptorSize() const noexcept
{
	return d->refusedDescriptorSize;
}

quint64 ConnectionSession::descriptorReceived() const noexcept
{
	return d->descriptorReceived;
}

quint64 ConnectionSession::descriptorTotal() const noexcept
{
	return d->descriptorTotal;
}

std::shared_ptr<FilesOnlyStorage>
ConnectionSession::filesOnlyStorage(client::Status& status) const noexcept
{
	Q_ASSERT(thread() == QThread::currentThread());
	status = client::Status::Ok;
	if (!filesOnly())
		return {};
	if (d->terminal)
		status = client::Status::Closed;
	else if (d->working || d->retiring)
		status = client::Status::Busy;
	else
		return d->filesOnly;
	return {};
}

std::shared_ptr<ConnectionStorage>
ConnectionSession::operationStorage(client::Status& status) const noexcept
{
	Q_ASSERT(thread() == QThread::currentThread());
	if (d->terminal)
		status = client::Status::Closed;
	else if (recoveryRequired())
		status = client::Status::RecoveryRequired;
	else if (!ready())
		status = client::Status::NotAdmitted;
	else if (d->working || d->retiring)
		status = client::Status::Busy;
	else {
		status = client::Status::Ok;
		return d->current;
	}
	return {};
}

std::shared_ptr<ConnectionStorage> ConnectionSession::storage() const
{
	return d->current;
}

HostWorkbench* ConnectionSession::workbench() const
{
	return ready() ? &d->current->inspector : nullptr;
}

bool ConnectionSession::reserveSessionsThrough(quint32 lastUsed)
{
	if (d->working || d->retiring || d->connected || d->opening ||
	    lastUsed == std::numeric_limits<quint32>::max())
		return false;
	d->lastSession = std::max(d->lastSession, lastUsed);
	return true;
}

void ConnectionSession::setWriteChunkBytes(int bytes)
{
	d->writeChunk = std::max(0, bytes);
	if (d->transport)
		d->transport->setWriteChunkBytes(d->writeChunk);
}

void ConnectionSession::setExchangeTimeout(int milliseconds)
{
	d->exchangeTimeout = std::max(1, milliseconds);
	if (d->transport)
		d->transport->setTimeout(d->exchangeTimeout);
}

ConnectionSession::IdentityState ConnectionSession::identityState() const noexcept
{
	return d->identityState;
}

QString ConnectionSession::deviceId() const
{
	return d->identityId;
}

QString ConnectionSession::deviceName() const
{
	return d->identityName;
}

QString ConnectionSession::deviceModel() const
{
	return d->identityModel;
}

QString ConnectionSession::deviceFirmware() const
{
	return d->identityFirmware;
}

QString ConnectionSession::identityProblem() const
{
	return d->identityProblem;
}

QString ConnectionSession::nameProblem(const QString& name) const
{
	const auto utf8 = name.toUtf8();
	const std::string_view text(utf8.constData(), static_cast<std::size_t>(utf8.size()));
	if (name.trimmed().isEmpty())
		return QStringLiteral("a name cannot be empty");
	if (static_cast<std::size_t>(utf8.size()) > ::telemetry::protocol::identity::maxText)
		return QStringLiteral("a name holds at most %1 bytes of UTF-8").arg(::telemetry::protocol::identity::maxText);
	if (!::telemetry::protocol::identity::validText(text))
		return QStringLiteral("a name cannot hold control characters");
	return {};
}

bool ConnectionSession::renaming() const noexcept
{
	return d->identityRename.step == Impl::IdentityRename::Step::Write ||
	       d->identityRename.step == Impl::IdentityRename::Step::ReadBack;
}

bool ConnectionSession::writeRefused(client::Input request) const noexcept
{
	// Only a WRITE (operation 4: u8 op, u32 index, ...) names a file to change.
	if (request.size() < 5 || request[0] != std::byte{4} || d->protocolWriteAllowed)
		return false;
	std::uint32_t index = 0;
	for (int i = 0; i < 4; ++i)
		index |= static_cast<std::uint32_t>(std::to_integer<unsigned>(request[1 + i])) << (8 * i);
	const auto& paths = d->discoveredPaths;
	if (index >= static_cast<std::uint32_t>(paths.size()))
		return false;
	// The SDK's own files - the descriptor, the session and batch files, the
	// identity, the values - are the protocol's, never a host's to rewrite.
	return paths[static_cast<qsizetype>(index)].startsWith(QStringLiteral("/telemetry/"));
}

bool ConnectionSession::rename(const QString& name)
{
	auto& state = *d;
	if (!state.connected || !filesSupported() || state.identityState != IdentityState::Known || renaming() ||
	    !nameProblem(name).isEmpty())
		return false;
	const auto text = [](const QString& value) { return value.toUtf8(); };
	const auto id = text(state.identityId), utf8 = text(name), model = text(state.identityModel),
	           firmware = text(state.identityFirmware);
	const auto view = [](const QByteArray& bytes) {
		return std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()));
	};
	std::array<std::byte, ::telemetry::protocol::identity::writtenBytes> encoded{};
	const auto written = ::telemetry::protocol::identity::write(
	    {view(id), view(utf8), view(model), view(firmware)}, encoded);
	if (written == 0)
		return false;
	auto& rename = state.identityRename;
	rename = {};
	rename.step = Impl::IdentityRename::Step::Write;
	rename.epoch = state.generation;
	rename.name = name;
	rename.bytes = QByteArray(reinterpret_cast<const char*>(encoded.data()), static_cast<qsizetype>(written));
	rename.due = true;
	pumpSoon();
	return true;
}

void ConnectionSession::process()
{
	const auto state = d;
	state->progress();
}

void ConnectionSession::pumpSoon() noexcept
{
	if (d->pumpQueued)
		return;
	d->pumpQueued = true;
	// The session is the context: nothing runs after it is gone.
	QTimer::singleShot(0, this, [this] {
		d->pumpQueued = false;
		process();
	});
}
} // namespace telemetry::integration::qt

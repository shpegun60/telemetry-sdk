/*
 * @file ConnectionSession.cpp
 * @brief One Qt transport/session owner with explicit same-peer recovery.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include <integration/qt/ConnectionSession.hpp>
#include <integration/qt/TcpTransport.hpp>
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
	quint64 refusedDescriptorSize = 0, descriptorReceived = 0, descriptorTotal = 0;
	QString host, peerAddress, retiredHost, retiredAddress, error;
	quint16 port = 0, retiredPort = 0;
	quint32 lastSession = 0;
	quint64 generation = 0;
	int writeChunk = 0, exchangeTimeout = 10000;
	bool connected = false, opening = false, terminal = false, working = false;
	bool retiring = false, closeRequested = false, nativeActive = false;

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
		transport.reset();
		retiring = false;
		if (owner)
			Q_EMIT owner->admissionChanged();
		notify();
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
		limitation = reason;
		refusedDescriptorSize = transport ? transport->refusedDescriptorSize() : 0;
		filesOnly = std::move(storage);
		connected = true;
		opening = false;
		error = detail.isEmpty() ? describe(reason, refusedDescriptorSize) : std::move(detail);
		timer.start();
		if (owner)
			Q_EMIT owner->admissionChanged();
		notify();
	}

	void filesOnlyDiscovered(quint64 epoch)
	{
		if (epoch != generation || !opening || !transport || !transport->filesOnly() || working)
			return;
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
	    [weak, epoch](const QByteArray& bytes, quint32 session, quint32 id) {
		    if (const auto value = weak.lock(); value && value->generation == epoch) {
			    if (value->current)
				    (void)value->current->receivePacket(bytes, session, id);
			    else if (value->filesOnly)
				    (void)value->filesOnly->receivePacket(bytes, session, id);
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

void ConnectionSession::process()
{
	const auto state = d;
	state->progress();
}
} // namespace telemetry::integration::qt

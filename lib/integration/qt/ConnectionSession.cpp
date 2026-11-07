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
	std::unique_ptr<TcpTransport> transport;
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

	void descriptor(quint64 epoch, QByteArray bytes)
	{
		if (epoch != generation || !opening || !transport || !transport->ready() || working)
			return;
		working = true;
		std::shared_ptr<ConnectionStorage> candidate;
		QString failure;
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
		else if (failure.isEmpty())
			failure = candidate->initialize(lastSession + 1, false);
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
		    if (const auto value = weak.lock();
		        value && value->generation == epoch && value->current)
			    (void)value->current->acceptPacket(session, id);
	    },
	    Qt::QueuedConnection);
	QObject::connect(
	    state->transport.get(), &DeviceTransport::packetReceived, this,
	    [weak, epoch](const QByteArray& bytes, quint32 session, quint32 id) {
		    if (const auto value = weak.lock();
		        value && value->generation == epoch && value->current)
			    (void)value->current->receivePacket(bytes, session, id);
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
	state->transport->connectTo(normalized, inputPort);
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

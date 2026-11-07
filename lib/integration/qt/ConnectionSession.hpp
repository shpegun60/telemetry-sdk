/*
 * @file ConnectionSession.hpp
 * @brief Optional Qt owner for the named Resource/TCP bootstrap profile.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_QT_CONNECTION_SESSION_HPP
#define TELEMETRY_INTEGRATION_QT_CONNECTION_SESSION_HPP
#pragma once

#include <integration/qt/ConnectionStorage.hpp>
#include <functional>

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
public:
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
	// Only after the core callback stack has returned. Borrowing UI adapters
	// consume owned outcome slots here; no Qt publication occurs inside core.
	void progressed();
	void stateChanged();

private:
	struct Impl;
	std::shared_ptr<Impl> d;
};
} // namespace telemetry::integration::qt
#endif

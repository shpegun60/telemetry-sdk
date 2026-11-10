/*
 * @file TcpTransport.hpp
 * @brief Qt TCP ownership, Resource discovery and unchanged Encoded packet transport.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_QT_TCP_TRANSPORT_HPP
#define TELEMETRY_INTEGRATION_QT_TCP_TRANSPORT_HPP
#pragma once

#include <integration/qt/DeviceTransport.hpp>
#include <memory>

namespace telemetry::integration::qt {
class TcpTransport final : public telemetry::integration::qt::DeviceTransport {

	Q_OBJECT

public:
	explicit TcpTransport(QObject* parent = nullptr);
	~TcpTransport() override;
	void connectTo(const QString& host, quint16 port);
	// Explicit optional named profile, preserving the original carrier method.
	void connectToWithReadMany(const QString& host, quint16 port, quint32 requestedSession);
	void connectToWithEncoded(const QString& host, quint16 port, quint32 requestedSession);
	QByteArray readManyCapability() const;
	QByteArray batchCapability() const;
	void close() override;
	bool ready() const noexcept override;
	bool canStage() const noexcept override;
	bool stage(telemetry::client::Packet packet) override;
	bool sendStaged() override;
	void discardStaged() override;
	bool locallyQuiescent() const noexcept override;
	QString peerAddress() const override;
	void setWriteChunkBytes(int bytes) noexcept override;
	void setTimeout(int milliseconds) noexcept override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace telemetry::integration::qt
#endif

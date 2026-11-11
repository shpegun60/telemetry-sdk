/*
 * @file DeviceTransport.hpp
 * @brief Application transport boundary for the application owner loop.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_QT_DEVICE_TRANSPORT_HPP
#define TELEMETRY_INTEGRATION_QT_DEVICE_TRANSPORT_HPP
#pragma once

#include <client/Backend.hpp>
#include <QtCore/QByteArray>
#include <QtCore/QObject>

namespace telemetry::integration::qt {
class DeviceTransport : public QObject {
	Q_OBJECT
public:
	explicit DeviceTransport(QObject* parent = nullptr) : QObject(parent)
	{}

	~DeviceTransport() override = default;
	virtual void close() = 0;
	virtual bool ready() const noexcept = 0;
	virtual bool canStage() const noexcept = 0;
	virtual bool stage(telemetry::client::Packet packet) = 0;
	virtual bool sendStaged() = 0;
	virtual void discardStaged() = 0;
	virtual bool locallyQuiescent() const noexcept = 0;
	virtual QString peerAddress() const = 0;
	virtual void setWriteChunkBytes(int bytes) noexcept = 0;
	virtual void setTimeout(int milliseconds) noexcept = 0;

	// Whether packets marked ReplyFrame::Resource can be sent after discovery,
	// for the device's files. A transport that cannot frame them refuses them.
	virtual bool supportsResourceReplies() const noexcept
	{
		return false;
	}

Q_SIGNALS:
	void descriptorReceived(QByteArray bytes);
	void packetAccepted(quint32 session, quint32 requestId);
	void packetReceived(QByteArray bytes, quint32 session, quint32 requestId);
	void transportError(QString message);
};
} // namespace telemetry::integration::qt
#endif

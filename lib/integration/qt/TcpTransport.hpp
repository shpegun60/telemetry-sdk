/*
 * @file TcpTransport.hpp
 * @brief Qt TCP ownership, Resource discovery and unchanged Encoded packet transport.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * A LinkTransport over a TcpLink and the TCP reference profile's framer. The
 * exchange rules are LinkTransport's; this class only says where to connect.
 */
#ifndef TELEMETRY_INTEGRATION_QT_TCP_TRANSPORT_HPP
#define TELEMETRY_INTEGRATION_QT_TCP_TRANSPORT_HPP
#pragma once

#include <integration/qt/LinkTransport.hpp>

namespace telemetry::integration::qt {
class TcpLink;

class TcpTransport final : public telemetry::integration::qt::LinkTransport {

	Q_OBJECT

public:
	explicit TcpTransport(QObject* parent = nullptr);
	~TcpTransport() override;
	void connectTo(const QString& host, quint16 port);
	// Explicit optional named profile, preserving the original carrier method.
	void connectToWithReadMany(const QString& host, quint16 port, quint32 requestedSession);
	void connectToWithEncoded(const QString& host, quint16 port, quint32 requestedSession);

private:
	TcpLink& tcp_;
};
} // namespace telemetry::integration::qt
#endif

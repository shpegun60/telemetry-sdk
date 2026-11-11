/*
 * @file TcpLink.hpp
 * @brief A Link over one QTcpSocket.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 */
#ifndef TELEMETRY_INTEGRATION_QT_TCP_LINK_HPP
#define TELEMETRY_INTEGRATION_QT_TCP_LINK_HPP
#pragma once

#include <integration/qt/Link.hpp>
#include <QtNetwork/QTcpSocket>

namespace telemetry::integration::qt {
class TcpLink final : public Link {
	Q_OBJECT
public:
	// readBufferBytes bounds what Qt holds unread; zero leaves it unbounded.
	explicit TcpLink(qint64 readBufferBytes, QObject* parent = nullptr);
	~TcpLink() override;
	void setTarget(const QString& host, quint16 port);
	QIODevice& device() noexcept override;
	void open() override;
	void close() override;
	bool quiescent() const noexcept override;
	QString peer() const override;

private:
	QTcpSocket socket_;
	QString host_;
	quint16 port_ = 0;
};
} // namespace telemetry::integration::qt
#endif

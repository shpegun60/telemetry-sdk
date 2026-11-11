/*
 * @file TcpLink.cpp
 * @brief A Link over one QTcpSocket.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 */
#include <integration/qt/TcpLink.hpp>

namespace telemetry::integration::qt {
TcpLink::TcpLink(qint64 readBufferBytes, QObject* parent) : Link(parent)
{
	socket_.setReadBufferSize(readBufferBytes);
	QObject::connect(&socket_, &QTcpSocket::connected, this, [this] {
		Q_EMIT opened();
	});
	// Qt reports a peer's close as an error first and a disconnection second;
	// both reach the transport in that order, which closes on the first.
	QObject::connect(&socket_, &QTcpSocket::disconnected, this, [this] {
		Q_EMIT lost(QStringLiteral("TCP peer disconnected"));
	});
	QObject::connect(&socket_, &QTcpSocket::errorOccurred, this,
	                 [this](QAbstractSocket::SocketError) {
		                 Q_EMIT lost(socket_.errorString());
	                 });
}

TcpLink::~TcpLink()
{
	QObject::disconnect(&socket_, nullptr, this, nullptr);
	socket_.abort();
}

void TcpLink::setTarget(const QString& host, quint16 port)
{
	host_ = host;
	port_ = port;
}

QIODevice& TcpLink::device() noexcept
{
	return socket_;
}

void TcpLink::open()
{
	socket_.connectToHost(host_, port_);
}

void TcpLink::close()
{
	socket_.abort();
}

bool TcpLink::quiescent() const noexcept
{
	return socket_.state() == QAbstractSocket::UnconnectedState && socket_.bytesToWrite() == 0;
}

QString TcpLink::peer() const
{
	return socket_.peerAddress().toString();
}
} // namespace telemetry::integration::qt

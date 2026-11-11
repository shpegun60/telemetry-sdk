/*
 * @file TcpTransport.cpp
 * @brief The TCP reference profile as a LinkTransport.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <integration/qt/TcpTransport.hpp>
#include <integration/qt/TcpLink.hpp>
#include <integration/tcp/ProfileFramer.hpp>

namespace telemetry::integration::qt {
namespace {
constexpr auto frameBytes =
    static_cast<qint64>(telemetry::integration::tcp::ProfileFramer::frameBytes);
}

TcpTransport::TcpTransport(QObject* parent)
    : LinkTransport(std::make_unique<TcpLink>(2 * frameBytes),
                    std::make_unique<telemetry::integration::tcp::ProfileFramer>(),
                    Options{60000, QStringLiteral("TCP")}, parent),
      tcp_(static_cast<TcpLink&>(link()))
{}

TcpTransport::~TcpTransport() = default;

void TcpTransport::connectTo(const QString& host, quint16 port)
{
	tcp_.setTarget(host, port);
	discover();
}

void TcpTransport::connectToWithReadMany(const QString& host, quint16 port,
                                         quint32 requestedSession)
{
	tcp_.setTarget(host, port);
	discoverWithReadMany(requestedSession);
}

void TcpTransport::connectToWithEncoded(const QString& host, quint16 port, quint32 requestedSession)
{
	tcp_.setTarget(host, port);
	discoverWithEncoded(requestedSession);
}
} // namespace telemetry::integration::qt

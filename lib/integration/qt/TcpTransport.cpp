/*
 * @file TcpTransport.cpp
 * @brief Bounded TCP stream I/O and sequential Descriptor Resource bootstrap.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <integration/qt/TcpTransport.hpp>
#include <integration/qt/DescriptorBootstrap.hpp>
#include <integration/tcp/Framing.hpp>
#include <resource/protocol/Packets.hpp>
#include <telemetry/type/WireTypes.hpp>
#include <wire/EncodedWire.hpp>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpSocket>
#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace telemetry::integration::qt {
namespace {
namespace tcp = telemetry::integration::tcp;
namespace rc = resource::protocol::client;
namespace ew = telemetry::protocol::encoded;
using Op = resource::protocol::Op;
using Input = telemetry::client::Input;

Input input(const QByteArray& bytes)
{
	return {reinterpret_cast<const std::byte*>(bytes.constData()),
	        static_cast<std::size_t>(bytes.size())};
}

QByteArray copy(Input bytes)
{
	return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}
} // namespace

struct TcpTransport::Impl {
	enum class Phase {
		Closed,
		Connecting,
		List,
		Stat,
		Read,
		Write,
		Encoded
	};
	TcpTransport& owner;
	QTcpSocket socket;
	QTimer timeout, discoveryTimeout;
	std::array<std::byte, 65568> rx{};
	tcp::FrameReader reader{rx, tcp::Mode::ResourceReply, Op::List};
	QByteArray incoming, tx;
	telemetry::integration::qt::DescriptorBootstrap discovery;
	qsizetype offset = 0;
	int writeChunk = std::numeric_limits<int>::max(), timeoutMs = 10000;
	quint32 session = 0, requestId = 0;
	Phase phase = Phase::Closed;
	bool awaiting = false, staged = false, writing = false, notified = false;
	quint64 generation = 0;

	template<class Callback>
	void later(int milliseconds, Callback callback)
	{
		const auto epoch = generation;
		QTimer::singleShot(milliseconds, &owner,
		                   [this, epoch, callback = std::move(callback)]() mutable {
			                   if (epoch == generation)
				                   callback();
		                   });
	}

	explicit Impl(TcpTransport& parent) : owner(parent)
	{
		timeout.setSingleShot(true);
		discoveryTimeout.setSingleShot(true);
		socket.setReadBufferSize(2 * static_cast<qint64>(rx.size()));
		QObject::connect(&socket, &QTcpSocket::connected, &owner, [this] {
			phase = Phase::List;
			scheduleBootstrap();
		});
		QObject::connect(&socket, &QTcpSocket::readyRead, &owner, [this] {
			receive();
		});
		QObject::connect(&socket, &QTcpSocket::bytesWritten, &owner, [this](qint64) {
			flush();
		});
		QObject::connect(&socket, &QTcpSocket::disconnected, &owner, [this] {
			if (phase != Phase::Closed)
				lost(QStringLiteral("TCP peer disconnected"));
		});
		QObject::connect(&socket, &QTcpSocket::errorOccurred, &owner,
		                 [this](QAbstractSocket::SocketError) {
			                 if (phase != Phase::Closed)
				                 lost(socket.errorString());
		                 });
		QObject::connect(&timeout, &QTimer::timeout, &owner, [this] {
			fail(QStringLiteral("TCP exchange timed out"));
		});
		QObject::connect(&discoveryTimeout, &QTimer::timeout, &owner, [this] {
			fail(QStringLiteral("Descriptor discovery timed out"));
		});
	}

	void close(bool invalidatePosted = true)
	{
		if (invalidatePosted)
			++generation;
		phase = Phase::Closed;
		timeout.stop();
		discoveryTimeout.stop();
		socket.abort();
		incoming.clear();
		tx.clear();
		reader.close();
		awaiting = staged = writing = false;
	}

	void fail(QString reason)
	{
		if (phase == Phase::Closed)
			return;
		// Keep complete replies already received on this connection ordered before
		// its error. Explicit close/reconnect still invalidates every old delivery.
		close(false);
		auto* target = &owner;
		later(0, [target, reason = std::move(reason)] {
			Q_EMIT target->transportError(reason);
		});
	}

	void lost(QString reason)
	{
		// EOF can arrive with the final complete reply still in Qt's RX buffer.
		// Ingest it before queuing failure; the core still applies its deadline.
		if (phase == Phase::Encoded)
			receive();
		fail(std::move(reason));
	}

	void connectProfile(const QString& host, quint16 port, quint32 requestedSession)
	{
		close();
		reader.reset();
		if (requestedSession)
			discovery.resetOptionalReadMany(requestedSession);
		else
			discovery.reset();
		phase = Phase::Connecting;
		timeout.start(timeoutMs);
		discoveryTimeout.start(60000);
		socket.connectToHost(host, port);
	}

	void scheduleBootstrap(int milliseconds = 0)
	{
		later(milliseconds, [this] {
			bootstrap();
		});
	}

	void bootstrap()
	{
		if (phase != Phase::List && phase != Phase::Stat && phase != Phase::Read && phase != Phase::Write)
			return;
		if (awaiting || !tx.isEmpty()) {
			scheduleBootstrap(1);
			return;
		}
		if (!incoming.isEmpty() || socket.bytesAvailable() != 0) {
			fail(QStringLiteral("Unsolicited Resource reply bytes"));
			return;
		}
		std::array<std::byte, 32> bytes{};
		const auto op = discovery.operation();
		const auto built = discovery.request(bytes);
		if (built.status != rc::BuildStatus::Ok) {
			fail(QStringLiteral("Cannot build Resource request"));
			return;
		}
		if (!reader.expect(tcp::Mode::ResourceReply, op)) {
			fail(QStringLiteral("Resource exchange is not at a frame boundary"));
			return;
		}
		tx = copy(Input{bytes}.first(built.written));
		offset = 0;
		awaiting = true;
		notified = false;
		timeout.start(timeoutMs);
		flush();
	}

	void flush()
	{
		if (writing || tx.isEmpty() || phase == Phase::Closed || staged)
			return;
		writing = true;
		unsigned budget = 64;
		while (offset < tx.size() && budget-- != 0) {
			const auto count = std::min<qsizetype>(tx.size() - offset, writeChunk);
			const auto accepted = socket.write(tx.constData() + offset, count);
			if (accepted < 0) {
				writing = false;
				fail(socket.errorString());
				return;
			}
			if (accepted == 0)
				break;
			offset += accepted;
		}
		const bool complete = offset == tx.size();
		const bool notify = complete && !notified && phase == Phase::Encoded;
		if (complete)
			notified = true;
		if (complete && socket.bytesToWrite() == 0)
			tx.clear();
		writing = false;
		if (!complete)
			later(1, [this] {
				flush();
			});
		if (notify) {
			auto* target = &owner;
			const auto sentSession = session, sentId = requestId;
			later(0, [target, sentSession, sentId] {
				Q_EMIT target->packetAccepted(sentSession, sentId);
			});
		}
	}

	bool resourceReply(Input bytes)
	{
		if (!discovery.accept(bytes))
			return false;
		phase = discovery.complete()                ? Phase::Encoded
		        : discovery.operation() == Op::List ? Phase::List
		        : discovery.operation() == Op::Stat ? Phase::Stat
		        : discovery.operation() == Op::Write ? Phase::Write
		                                            : Phase::Read;
		return true;
	}

	void receive()
	{
		if (phase == Phase::Closed || phase == Phase::Connecting)
			return;
		unsigned budget = 32;
		while (budget-- != 0 && (socket.bytesAvailable() != 0 || !incoming.isEmpty())) {
			if (incoming.isEmpty())
				incoming = socket.read(4096);
			if (incoming.isEmpty())
				break;
			const auto fed = reader.feed(input(incoming));
			incoming.remove(0, static_cast<qsizetype>(fed.consumed));
			if (fed.status == tcp::Status::NeedMore)
				continue;
			if (fed.status != tcp::Status::Ready) {
				fail(QStringLiteral("Malformed or oversized TCP frame"));
				return;
			}
			if (phase == Phase::Encoded) {
				const auto parsed = ew::parse(reader.frame());
				if (!parsed || !parsed.header.reply) {
					fail(QStringLiteral("Invalid Encoded reply"));
					return;
				}
				const auto bytes = copy(reader.frame());
				const auto header = parsed.header;
				reader.release();
				if (awaiting && header.session == session && header.requestId == requestId) {
					awaiting = false;
					timeout.stop();
				}
				auto* target = &owner;
				later(0, [target, bytes, header] {
					Q_EMIT target->packetReceived(bytes, header.session, header.requestId);
				});
			} else {
				// A Resource packet has no identity. Never reinterpret a buffered tail
				// as the reply to a next request that has not yet been sent.
				if (!awaiting || !incoming.isEmpty() || socket.bytesAvailable() != 0 ||
				    !resourceReply(reader.frame())) {
					fail(QStringLiteral("Invalid Descriptor discovery reply"));
					return;
				}
				awaiting = false;
				timeout.stop();
				reader.release();
				if (phase == Phase::Encoded) {
					discoveryTimeout.stop();
					if (!reader.expect(tcp::Mode::EncodedReply)) {
						fail(QStringLiteral("Descriptor did not end at a frame boundary"));
						return;
					}
					auto* target = &owner;
					const auto bytes = discovery.descriptor();
					later(0, [this, target, bytes] {
						if (phase == Phase::Encoded)
							Q_EMIT target->descriptorReceived(bytes);
					});
				} else {
					scheduleBootstrap();
				}
				return;
			}
		}
		if (socket.bytesAvailable() != 0 || !incoming.isEmpty())
			later(0, [this] {
				receive();
			});
	}
};

TcpTransport::TcpTransport(QObject* parent)
    : telemetry::integration::qt::DeviceTransport(parent), impl_(std::make_unique<Impl>(*this))
{}

TcpTransport::~TcpTransport()
{
	impl_->close();
}

void TcpTransport::connectTo(const QString& host, quint16 port)
{
	impl_->connectProfile(host, port, 0);
}

void TcpTransport::connectToWithReadMany(const QString& host, quint16 port, quint32 requestedSession)
{
	if (!requestedSession) {
		impl_->close();
		Q_EMIT transportError(QStringLiteral("Optional ReadMany discovery requires a nonzero session"));
		return;
	}
	impl_->connectProfile(host, port, requestedSession);
}

QByteArray TcpTransport::readManyCapability() const
{
	return ready() ? impl_->discovery.capability() : QByteArray{};
}

void TcpTransport::close()
{
	impl_->close();
}

bool TcpTransport::ready() const noexcept
{
	return impl_->phase == Impl::Phase::Encoded;
}

bool TcpTransport::canStage() const noexcept
{
	return ready() && !impl_->awaiting && !impl_->staged && impl_->tx.isEmpty() &&
	       impl_->socket.bytesToWrite() == 0;
}

bool TcpTransport::stage(telemetry::client::Packet packet)
{
	if (!canStage() || !packet || packet.bytes.size() > impl_->rx.size())
		return false;
	impl_->tx = copy(packet.bytes);
	impl_->session = packet.session;
	impl_->requestId = packet.requestId;
	impl_->offset = 0;
	impl_->staged = true;
	impl_->notified = false;
	return true;
}

bool TcpTransport::sendStaged()
{
	if (!impl_->staged || !ready())
		return false;
	impl_->staged = false;
	impl_->awaiting = true;
	impl_->timeout.start(impl_->timeoutMs);
	impl_->flush();
	return ready();
}

void TcpTransport::discardStaged()
{
	if (impl_->staged) {
		impl_->staged = false;
		impl_->tx.clear();
	}
}

bool TcpTransport::locallyQuiescent() const noexcept
{
	return impl_->phase == Impl::Phase::Closed &&
	       impl_->socket.state() == QAbstractSocket::UnconnectedState && impl_->tx.isEmpty() &&
	       impl_->incoming.isEmpty() && impl_->socket.bytesToWrite() == 0;
}

QString TcpTransport::peerAddress() const
{
	return impl_->socket.peerAddress().toString();
}

void TcpTransport::setWriteChunkBytes(int bytes) noexcept
{
	impl_->writeChunk = bytes > 0 ? bytes : std::numeric_limits<int>::max();
}

void TcpTransport::setTimeout(int milliseconds) noexcept
{
	impl_->timeoutMs = std::clamp(milliseconds, 1, 60000);
}
} // namespace telemetry::integration::qt

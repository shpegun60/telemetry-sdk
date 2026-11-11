/*
 * @file LinkTransport.cpp
 * @brief Bounded link I/O and sequential Descriptor Resource bootstrap.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * This is TcpTransport's former implementation with its socket replaced by a
 * Link and its FrameReader by a Framer. The order of every step is unchanged;
 * tests/link_transport holds the trace that proves it.
 */
#include <integration/qt/LinkTransport.hpp>
#include <integration/qt/DescriptorBootstrap.hpp>
#include <resource/protocol/Packets.hpp>
#include <telemetry/type/WireTypes.hpp>
#include <wire/EncodedWire.hpp>
#include <QtCore/QTimer>
#include <algorithm>
#include <array>
#include <limits>

namespace telemetry::integration::qt {
namespace {
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

// The link is dereferenced while the transport is built; a missing one is a
// programming error, reported before anything is connected.
telemetry::integration::qt::Link&
required(const std::unique_ptr<telemetry::integration::qt::Link>& link)
{
	Q_ASSERT(link);
	return *link;
}
} // namespace

struct LinkTransport::Impl {
	enum class Phase {
		Closed,
		Connecting,
		List,
		Stat,
		Read,
		Write,
		Encoded,
		// Discovery listed the files correctly but found no usable descriptor:
		// only Resource requests are sent, for the device's files.
		FilesOnly
	};
	LinkTransport& owner;
	std::unique_ptr<Link> link;
	std::unique_ptr<telemetry::integration::link::Framer> framer;
	QIODevice& device;
	const Options options;
	QTimer timeout, discoveryTimeout;
	QByteArray incoming, tx;
	telemetry::integration::qt::DescriptorBootstrap discovery;
	qsizetype offset = 0;
	int writeChunk = std::numeric_limits<int>::max(), timeoutMs = 10000;
	quint32 session = 0, requestId = 0;
	Phase phase = Phase::Closed;
	bool awaiting = false, staged = false, writing = false, notified = false;
	// A Resource request staged or outstanding after discovery. The framer reads
	// Resource replies only while it is set and returns to Encoded right after.
	bool resourceStaged = false, resourceExchange = false;
	std::uint8_t resourceOperation = 0;
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

	Impl(LinkTransport& parent, std::unique_ptr<Link> channel,
	     std::unique_ptr<telemetry::integration::link::Framer> frames, Options settings)
	    : owner(parent), link(std::move(channel)), framer(std::move(frames)),
	      device(required(link).device()), options(std::move(settings))
	{
		Q_ASSERT(link && framer);
		timeout.setSingleShot(true);
		discoveryTimeout.setSingleShot(true);
		QObject::connect(link.get(), &Link::opened, &owner, [this] {
			phase = Phase::List;
			scheduleBootstrap();
		});
		QObject::connect(&device, &QIODevice::readyRead, &owner, [this] {
			receive();
		});
		QObject::connect(&device, &QIODevice::bytesWritten, &owner, [this](qint64) {
			flush();
		});
		QObject::connect(link.get(), &Link::lost, &owner, [this](const QString& reason) {
			if (phase != Phase::Closed)
				lost(reason);
		});
		QObject::connect(&timeout, &QTimer::timeout, &owner, [this] {
			fail(options.medium + QStringLiteral(" exchange timed out"));
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
		link->close();
		incoming.clear();
		tx.clear();
		framer->close();
		awaiting = staged = writing = false;
		resourceStaged = resourceExchange = false;
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
		// EOF can arrive with the final complete reply still in the link's buffer.
		// Ingest it before queuing failure; the core still applies its deadline.
		if (phase == Phase::Encoded)
			receive();
		fail(std::move(reason));
	}

	void connectProfile(quint32 requestedSession, bool batch = false)
	{
		close();
		framer->reset();
		if (requestedSession && batch)
			discovery.resetOptionalEncoded(requestedSession);
		else if (requestedSession)
			discovery.resetOptionalReadMany(requestedSession);
		else
			discovery.reset();
		phase = Phase::Connecting;
		timeout.start(timeoutMs);
		discoveryTimeout.start(options.discoveryTimeoutMs);
		link->open();
	}

	// The medium's bytes for one packet, or empty when the framer refuses it.
	QByteArray encode(Input packet)
	{
		const auto bound = framer->maxEncodedBytes(packet.size());
		if (bound == 0 || packet.empty())
			return {};
		QByteArray bytes(static_cast<qsizetype>(bound), Qt::Uninitialized);
		const resource::Output out{reinterpret_cast<std::byte*>(bytes.data()), bound};
		const auto written = framer->encode(packet, out);
		if (written == 0 || written > bound)
			return {};
		bytes.truncate(static_cast<qsizetype>(written));
		return bytes;
	}

	void scheduleBootstrap(int milliseconds = 0)
	{
		later(milliseconds, [this] {
			bootstrap();
		});
	}

	void bootstrap()
	{
		if (phase != Phase::List && phase != Phase::Stat && phase != Phase::Read &&
		    phase != Phase::Write)
			return;
		if (awaiting || !tx.isEmpty()) {
			scheduleBootstrap(1);
			return;
		}
		if (!incoming.isEmpty() || device.bytesAvailable() != 0) {
			fail(QStringLiteral("Unsolicited Resource reply bytes"));
			return;
		}
		std::array<std::byte, 32> bytes{};
		const auto op = discovery.operation();
		const auto built = discovery.request(bytes);
		const auto request = built.status == rc::BuildStatus::Ok
		                         ? encode(Input{bytes}.first(built.written))
		                         : QByteArray{};
		if (request.isEmpty()) {
			fail(QStringLiteral("Cannot build Resource request"));
			return;
		}
		if (!framer->expect(telemetry::integration::link::Expect::ResourceReply, op)) {
			fail(QStringLiteral("Resource exchange is not at a frame boundary"));
			return;
		}
		tx = request;
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
			const auto accepted = device.write(tx.constData() + offset, count);
			if (accepted < 0) {
				writing = false;
				fail(device.errorString());
				return;
			}
			if (accepted == 0)
				break;
			offset += accepted;
		}
		const bool complete = offset == tx.size();
		const bool notify =
		    complete && !notified && (phase == Phase::Encoded || phase == Phase::FilesOnly);
		if (complete)
			notified = true;
		if (complete && device.bytesToWrite() == 0)
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

	void enterFilesOnly()
	{
		awaiting = false;
		timeout.stop();
		discoveryTimeout.stop();
		framer->release();
		if (!framer->expect(telemetry::integration::link::Expect::EncodedReply, Op::Read)) {
			fail(QStringLiteral("Discovery did not end at a frame boundary"));
			return;
		}
		phase = Phase::FilesOnly;
		auto* target = &owner;
		later(0, [this, target] {
			if (phase == Phase::FilesOnly)
				Q_EMIT target->filesOnlyReached();
		});
	}

	bool resourceReply(Input bytes)
	{
		if (!discovery.accept(bytes))
			return false;
		phase = discovery.complete()                 ? Phase::Encoded
		        : discovery.operation() == Op::List  ? Phase::List
		        : discovery.operation() == Op::Stat  ? Phase::Stat
		        : discovery.operation() == Op::Write ? Phase::Write
		                                             : Phase::Read;
		return true;
	}

	void receive()
	{
		using telemetry::integration::link::Status;
		if (phase == Phase::Closed || phase == Phase::Connecting)
			return;
		unsigned budget = 32;
		while (budget-- != 0 && (device.bytesAvailable() != 0 || !incoming.isEmpty())) {
			if (incoming.isEmpty())
				incoming = device.read(4096);
			if (incoming.isEmpty())
				break;
			const auto fed = framer->feed(input(incoming));
			incoming.remove(0, static_cast<qsizetype>(fed.consumed));
			if (fed.status == Status::NeedMore)
				continue;
			if (fed.status != Status::Ready) {
				fail(QStringLiteral("Malformed or oversized ") + options.medium +
				     QStringLiteral(" frame"));
				return;
			}
			if ((phase == Phase::Encoded || phase == Phase::FilesOnly) && resourceExchange) {
				// A Resource reply has no identity; it answers the one outstanding
				// request. Bytes beyond it were never asked for.
				if (!awaiting || !incoming.isEmpty() || device.bytesAvailable() != 0) {
					fail(QStringLiteral("Unsolicited bytes after a Resource reply"));
					return;
				}
				const auto bytes = copy(framer->frame());
				framer->release();
				resourceExchange = awaiting = false;
				timeout.stop();
				if (!framer->expect(telemetry::integration::link::Expect::EncodedReply, Op::Read)) {
					fail(QStringLiteral("Resource reply did not end at a frame boundary"));
					return;
				}
				auto* target = &owner;
				const auto sentSession = session, sentId = requestId;
				later(0, [target, bytes, sentSession, sentId] {
					Q_EMIT target->packetReceived(bytes, sentSession, sentId);
				});
				return;
			}
			if (phase == Phase::FilesOnly) {
				fail(QStringLiteral("Unsolicited bytes from a files-only device"));
				return;
			}
			if (phase == Phase::Encoded) {
				const auto parsed = ew::parse(framer->frame());
				if (!parsed || !parsed.header.reply) {
					fail(QStringLiteral("Invalid Encoded reply"));
					return;
				}
				const auto bytes = copy(framer->frame());
				const auto header = parsed.header;
				framer->release();
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
				if (!awaiting || !incoming.isEmpty() || device.bytesAvailable() != 0) {
					fail(QStringLiteral("Invalid Descriptor discovery reply"));
					return;
				}
				if (!resourceReply(framer->frame())) {
					// A device whose list was read correctly stays open for its
					// files; only a broken protocol closes it.
					const auto refusal = discovery.refusal();
					if (discovery.listed() && refusal != DescriptorBootstrap::Refusal::None &&
					    refusal != DescriptorBootstrap::Refusal::Protocol)
						enterFilesOnly();
					else
						fail(QStringLiteral("Invalid Descriptor discovery reply"));
					return;
				}
				awaiting = false;
				timeout.stop();
				framer->release();
				if (phase == Phase::Encoded) {
					discoveryTimeout.stop();
					if (!framer->expect(telemetry::integration::link::Expect::EncodedReply,
					                    Op::Read)) {
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
		if (device.bytesAvailable() != 0 || !incoming.isEmpty())
			later(0, [this] {
				receive();
			});
	}
};

LinkTransport::LinkTransport(std::unique_ptr<Link> link,
                             std::unique_ptr<telemetry::integration::link::Framer> framer,
                             Options options, QObject* parent)
    : telemetry::integration::qt::DeviceTransport(parent),
      impl_(std::make_unique<Impl>(*this, std::move(link), std::move(framer), std::move(options)))
{}

LinkTransport::~LinkTransport()
{
	impl_->close();
}

Link& LinkTransport::link() noexcept
{
	return *impl_->link;
}

void LinkTransport::discover()
{
	impl_->connectProfile(0);
}

void LinkTransport::discoverWithReadMany(quint32 requestedSession)
{
	if (!requestedSession) {
		impl_->close();
		Q_EMIT transportError(
		    QStringLiteral("Optional ReadMany discovery requires a nonzero session"));
		return;
	}
	impl_->connectProfile(requestedSession);
}

QByteArray LinkTransport::readManyCapability() const
{
	return ready() ? impl_->discovery.capability() : QByteArray{};
}

void LinkTransport::discoverWithEncoded(quint32 requestedSession)
{
	if (!requestedSession) {
		impl_->close();
		Q_EMIT transportError(
		    QStringLiteral("Optional Encoded discovery requires a nonzero session"));
		return;
	}
	impl_->connectProfile(requestedSession, true);
}

QByteArray LinkTransport::batchCapability() const
{
	return ready() ? impl_->discovery.batchCapability() : QByteArray{};
}

void LinkTransport::close()
{
	impl_->close();
}

bool LinkTransport::ready() const noexcept
{
	return impl_->phase == Impl::Phase::Encoded;
}

bool LinkTransport::filesOnly() const noexcept
{
	return impl_->phase == Impl::Phase::FilesOnly;
}

DescriptorBootstrap::Refusal LinkTransport::discoveryRefusal() const noexcept
{
	return impl_->discovery.refusal();
}

QStringList LinkTransport::discoveredPaths() const
{
	return impl_->discovery.paths();
}

quint64 LinkTransport::refusedDescriptorSize() const noexcept
{
	return impl_->discovery.descriptorSize();
}

bool LinkTransport::canStage() const noexcept
{
	return (ready() || filesOnly()) && !impl_->awaiting && !impl_->staged && impl_->tx.isEmpty() &&
	       impl_->device.bytesToWrite() == 0;
}

bool LinkTransport::stage(telemetry::client::Packet packet)
{
	if (!canStage() || !packet)
		return false;
	const bool resource = packet.reply == telemetry::client::ReplyFrame::Resource;
	// Only List, Stat and Read are framed after discovery, and a files-only
	// device takes nothing else.
	if ((resource && (packet.resourceOperation < 1 || packet.resourceOperation > 3)) ||
	    (!resource && filesOnly()))
		return false;
	auto bytes = impl_->encode(packet.bytes);
	if (bytes.isEmpty())
		return false;
	// The framer is switched only once the packet is really sent: a staged
	// packet may still be discarded, and then nothing about the stream changed.
	impl_->resourceStaged = resource;
	impl_->resourceOperation = packet.resourceOperation;
	impl_->tx = std::move(bytes);
	impl_->session = packet.session;
	impl_->requestId = packet.requestId;
	impl_->offset = 0;
	impl_->staged = true;
	impl_->notified = false;
	return true;
}

bool LinkTransport::sendStaged()
{
	if (!impl_->staged || !(ready() || filesOnly()))
		return false;
	if (impl_->resourceStaged &&
	    !impl_->framer->expect(telemetry::integration::link::Expect::ResourceReply,
	                           static_cast<resource::protocol::Op>(impl_->resourceOperation))) {
		// Encoded traffic leaves the framer alone, exactly as before. A Resource
		// request switches it at a frame boundary; anything else is a broken stream.
		impl_->fail(QStringLiteral("Resource request is not at a frame boundary"));
		return false;
	}
	impl_->staged = false;
	impl_->awaiting = true;
	impl_->resourceExchange = impl_->resourceStaged;
	impl_->resourceStaged = false;
	impl_->timeout.start(impl_->timeoutMs);
	impl_->flush();
	return ready() || filesOnly();
}

void LinkTransport::discardStaged()
{
	if (impl_->staged) {
		impl_->staged = false;
		impl_->tx.clear();
		impl_->resourceStaged = false;
	}
}

bool LinkTransport::locallyQuiescent() const noexcept
{
	return impl_->phase == Impl::Phase::Closed && impl_->link->quiescent() && impl_->tx.isEmpty() &&
	       impl_->incoming.isEmpty();
}

QString LinkTransport::peerAddress() const
{
	return impl_->link->peer();
}

void LinkTransport::setWriteChunkBytes(int bytes) noexcept
{
	impl_->writeChunk = bytes > 0 ? bytes : std::numeric_limits<int>::max();
}

bool LinkTransport::supportsResourceReplies() const noexcept
{
	return true;
}

void LinkTransport::setTimeout(int milliseconds) noexcept
{
	impl_->timeoutMs = std::clamp(milliseconds, 1, 60000);
}
} // namespace telemetry::integration::qt

// LinkTransport over a link that is not a socket: an in-memory device answered
// by the native TCP profile server. Proves discovery, loss, broken frames,
// deadlines and the medium's name in errors do not depend on TCP. MIT.
#include <integration/qt/LinkTransport.hpp>
#include <integration/tcp/ProfileFramer.hpp>
#include <integration/tcp/Server.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QIODevice>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
namespace qt = telemetry::integration::qt;
namespace tcp = telemetry::integration::tcp;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Link channel check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

bool await(const std::function<bool()>& done, int milliseconds = 5000)
{
	QElapsedTimer deadline;
	deadline.start();
	while (!done() && deadline.elapsed() < milliseconds) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	return done();
}

struct Device {
	std::uint32_t read() noexcept
	{
		return 42;
	}
} device;

constexpr telemetry::FieldTable localFields{telemetry::field<&Device::read>("Value", device)};
constexpr telemetry::FieldCatalogTable fields{telemetry::group("test", localFields)};
constexpr telemetry::CommandCatalogTable commands{};
constexpr telemetry::ServiceCatalogTable services{};
constexpr telemetry::Model model{fields, commands, services};
constexpr telemetry::descriptor::Descriptor descriptor{model};
constexpr auto packed = telemetry::descriptor::packDescriptor<descriptor>();

// Bytes the transport writes collect in `written`; bytes pushed in are read.
class MemoryDevice final : public QIODevice {
public:
	QByteArray incoming, written;
	unsigned writes = 0;

	bool isSequential() const override
	{
		return true;
	}

	qint64 bytesAvailable() const override
	{
		return incoming.size() + QIODevice::bytesAvailable();
	}

	void push(const QByteArray& bytes)
	{
		incoming += bytes;
		Q_EMIT readyRead();
	}

protected:
	qint64 readData(char* data, qint64 maximum) override
	{
		const auto count = std::min<qint64>(maximum, incoming.size());
		std::memcpy(data, incoming.constData(), static_cast<std::size_t>(count));
		incoming.remove(0, static_cast<qsizetype>(count));
		return count;
	}

	qint64 writeData(const char* data, qint64 bytes) override
	{
		written.append(data, static_cast<qsizetype>(bytes));
		++writes;
		QTimer::singleShot(0, this, [this, bytes] {
			Q_EMIT bytesWritten(bytes);
		});
		return bytes;
	}
};

class FakeLink final : public qt::Link {
public:
	MemoryDevice memory;
	unsigned opens = 0, closes = 0;

	QIODevice& device() noexcept override
	{
		return memory;
	}

	void open() override
	{
		++opens;
		memory.open(QIODevice::ReadWrite | QIODevice::Unbuffered);
		QTimer::singleShot(0, this, [this] {
			if (memory.isOpen())
				Q_EMIT opened();
		});
	}

	void close() override
	{
		++closes;
		memory.close();
		memory.incoming.clear();
		memory.written.clear();
	}

	bool quiescent() const noexcept override
	{
		return !memory.isOpen();
	}

	QString peer() const override
	{
		return memory.isOpen() ? QStringLiteral("memory:1") : QString{};
	}
};

class FakeTransport final : public qt::LinkTransport {
public:
	FakeLink* fake;
	QStringList errors;
	QByteArray descriptorBytes;

	FakeTransport()
	    : qt::LinkTransport(std::make_unique<FakeLink>(), std::make_unique<tcp::ProfileFramer>(),
	                        Options{60000, QStringLiteral("MEMORY")}),
	      fake(static_cast<FakeLink*>(&link()))
	{
		QObject::connect(this, &DeviceTransport::transportError, this,
		                 [this](const QString& message) {
			                 errors << message;
		                 });
		QObject::connect(this, &DeviceTransport::descriptorReceived, this,
		                 [this](const QByteArray& bytes) {
			                 descriptorBytes = bytes;
		                 });
	}

	using qt::LinkTransport::discover;
	using qt::LinkTransport::discoverWithReadMany;
};

// The device end: answers whatever the transport wrote with the native server.
class Peer {
public:
	std::unique_ptr<tcp::ReadManyProfile> profile;
	std::unique_ptr<tcp::Server> server;
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 8192> rx{}, tx{};
	bool silent = false;
	QByteArray pending;

	Peer()
	{
		profile = std::make_unique<tcp::ReadManyProfile>(packed, descriptor.fingerprint());
		server = std::make_unique<tcp::Server>(model.view(), workspace, *profile, rx, tx);
	}

	void serve(MemoryDevice& memory)
	{
		pending += memory.written;
		memory.written.clear();
		QByteArray replies;
		while (!pending.isEmpty()) {
			const auto result =
			    server->receive({reinterpret_cast<const std::byte*>(pending.constData()),
			                     static_cast<std::size_t>(pending.size())});
			pending.remove(0, static_cast<qsizetype>(result.consumed));
			if (result.status == tcp::Status::NeedMore)
				break;
			const auto bytes = server->reply();
			if (!silent)
				replies.append(reinterpret_cast<const char*>(bytes.data()),
				               static_cast<qsizetype>(bytes.size()));
			server->releaseReply();
		}
		if (!replies.isEmpty())
			memory.push(replies);
	}
};

bool discovered(FakeTransport& transport, Peer& peer)
{
	return await([&] {
		if (transport.fake->memory.isOpen())
			peer.serve(transport.fake->memory);
		return transport.ready() || !transport.errors.isEmpty();
	});
}

void discovery()
{
	FakeTransport transport;
	Peer peer;
	CHECK(transport.locallyQuiescent() && transport.peerAddress().isEmpty());
	transport.setWriteChunkBytes(1);
	transport.discoverWithReadMany(5);
	CHECK(!transport.ready() && transport.fake->opens == 1);
	CHECK(discovered(transport, peer));
	CHECK(transport.ready() && transport.errors.isEmpty());
	CHECK(await([&] {
		return !transport.descriptorBytes.isEmpty();
	}));
	CHECK(transport.descriptorBytes == QByteArray(reinterpret_cast<const char*>(packed.data()),
	                                              static_cast<qsizetype>(packed.size())));
	CHECK(!transport.readManyCapability().isEmpty() && transport.batchCapability().isEmpty());
	CHECK(transport.peerAddress() == QStringLiteral("memory:1"));
	// Chunked to one byte: every request went out a byte per write.
	CHECK(transport.fake->memory.writes > 3 * 8);
	CHECK(transport.canStage() && !transport.locallyQuiescent());
	transport.close();
	CHECK(!transport.ready() && transport.locallyQuiescent() && transport.fake->closes >= 2);
	CHECK(transport.readManyCapability().isEmpty());
}

void loss()
{
	FakeTransport transport;
	Peer peer;
	transport.discover();
	CHECK(discovered(transport, peer) && transport.ready());
	// The link reports its loss more than once; the transport reports it once.
	Q_EMIT transport.fake->lost(QStringLiteral("memory link gone"));
	Q_EMIT transport.fake->lost(QStringLiteral("memory link gone again"));
	CHECK(!transport.ready() && transport.locallyQuiescent());
	CHECK(await([&] {
		return !transport.errors.isEmpty();
	}));
	QCoreApplication::processEvents();
	CHECK(transport.errors == QStringList{QStringLiteral("memory link gone")});
}

void brokenFrame()
{
	FakeTransport transport;
	Peer peer;
	transport.discover();
	CHECK(discovered(transport, peer) && transport.ready());
	transport.fake->memory.push(QByteArray(48, '\x5a'));
	CHECK(await([&] {
		return !transport.errors.isEmpty();
	}));
	CHECK(transport.errors == QStringList{QStringLiteral("Malformed or oversized MEMORY frame")});
	CHECK(!transport.ready() && transport.locallyQuiescent());
}

void deadline()
{
	FakeTransport transport;
	Peer peer;
	peer.silent = true;
	transport.setTimeout(50);
	transport.discover();
	CHECK(discovered(transport, peer));
	CHECK(transport.errors == QStringList{QStringLiteral("MEMORY exchange timed out")});
	CHECK(!transport.ready() && transport.locallyQuiescent());
}

void refusal()
{
	FakeTransport transport;
	transport.discoverWithReadMany(0); // Refused at once, without opening.
	CHECK(transport.errors ==
	      QStringList{QStringLiteral("Optional ReadMany discovery requires a nonzero session")});
	CHECK(transport.fake->opens == 0 && transport.locallyQuiescent());
}
} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	discovery();
	loss();
	brokenFrame();
	deadline();
	refusal();
	std::printf("Link channel: %u checks, 0 failures\n", checks);
	return 0;
}

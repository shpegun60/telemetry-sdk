// Public Qt Session consumer against a native localhost server, no UX. MIT.
#include <integration/qt/ConnectionSession.hpp>
#include <integration/tcp/Server.hpp>
#include <resource/BytesFile.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>
#include <QThread>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
namespace qt = telemetry::integration::qt;
namespace tcp = telemetry::integration::tcp;
namespace cl = telemetry::client;
namespace rm = telemetry::integration::read_many;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Qt ReadMany session check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

bool await(const std::function<bool()>& done)
{
	QElapsedTimer deadline;
	deadline.start();
	while (!done() && deadline.elapsed() < 5000) {
		QCoreApplication::processEvents();
		QThread::msleep(1);
	}
	return done();
}

struct Device {
	unsigned reads = 0;

	std::uint32_t read() noexcept
	{
		++reads;
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
constexpr resource::BytesFile descriptorFile{packed};
constexpr std::array<std::byte, 4> placeholderBytes{};
constexpr resource::BytesFile placeholder{placeholderBytes};
const auto legacyFiles =
    resource::filesystem(resource::file("/telemetry/descriptor.bin", descriptorFile));
const auto sessionOnlyFiles =
    resource::filesystem(resource::file("/telemetry/descriptor.bin", descriptorFile),
                         resource::file("/telemetry/session.bin", placeholder));
const auto capabilityOnlyFiles =
    resource::filesystem(resource::file("/telemetry/descriptor.bin", descriptorFile),
                         resource::file("/telemetry/read-many.bin", placeholder));
enum class Mode {
	Modern,
	Legacy,
	MissingCapability,
	MissingSession,
	WrongFingerprint
};

class Peer {
public:
	QTcpServer listener;
	QPointer<QTcpSocket> socket;
	std::unique_ptr<tcp::ReadManyProfile> profile;
	std::unique_ptr<tcp::Server> server;
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 8192> rx{}, tx{};
	QByteArray pending;
	unsigned connections = 0, encoded = 0;

	explicit Peer(Mode mode = Mode::Modern)
	{
		CHECK(listener.listen(QHostAddress::LocalHost, 0));
		QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this, mode] {
			auto* candidate = listener.nextPendingConnection();
			CHECK(candidate && !socket);
			socket = candidate;
			++connections;
			if (mode == Mode::Modern || mode == Mode::WrongFingerprint) {
				profile = std::make_unique<tcp::ReadManyProfile>(
				    packed, descriptor.fingerprint() ^ (mode == Mode::WrongFingerprint ? 1u : 0u));
				server = std::make_unique<tcp::Server>(model.view(), workspace, *profile, rx, tx);
			} else {
				const auto files = mode == Mode::Legacy              ? legacyFiles.view()
				                   : mode == Mode::MissingCapability ? sessionOnlyFiles.view()
				                                                     : capabilityOnlyFiles.view();
				server = std::make_unique<tcp::Server>(model.view(), workspace, files, rx, tx);
			}
			QObject::connect(candidate, &QTcpSocket::readyRead, &listener, [this] {
				read();
			});
			QObject::connect(candidate, &QTcpSocket::disconnected, &listener, [this, candidate] {
				if (socket != candidate)
					return;
				server->close();
				server.reset();
				profile.reset();
				pending.clear();
				socket.clear();
				candidate->deleteLater();
			});
			if (candidate->bytesAvailable())
				read();
		});
	}

	~Peer()
	{
		if (socket) {
			QObject::disconnect(socket, nullptr, &listener, nullptr);
			socket->abort();
		}
	}

	quint16 port() const
	{
		return listener.serverPort();
	}

	void read()
	{
		CHECK(socket && server);
		CHECK(pending.size() + socket->bytesAvailable() <= 16384);
		pending += socket->readAll();
		while (!pending.isEmpty()) {
			const auto result =
			    server->receive({reinterpret_cast<const std::byte*>(pending.constData()),
			                     static_cast<std::size_t>(pending.size())});
			CHECK(result.consumed <= static_cast<std::size_t>(pending.size()));
			pending.remove(0, static_cast<qsizetype>(result.consumed));
			if (result.status == tcp::Status::NeedMore)
				break;
			CHECK(result.status == tcp::Status::Ready);
			const auto bytes = server->reply();
			if (!bytes.empty() && bytes[0] == std::byte{'T'})
				++encoded;
			CHECK(socket->write(reinterpret_cast<const char*>(bytes.data()),
			                    static_cast<qint64>(bytes.size())) ==
			      static_cast<qint64>(bytes.size()));
			server->releaseReply(); // QTcpSocket has copied the complete reply.
		}
	}
};

void modern()
{
	const auto before = device.reads;
	Peer peer;
	qt::ConnectionSession connection;
	connection.setWriteChunkBytes(1);
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection.ready() || !connection.lastError().isEmpty();
	}));
	CHECK(connection.ready() && connection.readManySupported());
	CHECK(peer.profile && peer.profile->bound() && !peer.profile->frozen());
	CHECK(device.reads == before && peer.encoded == 0); // Discovery cannot probe a getter.
	const auto cap = connection.readManyCapability();
	CHECK(cap && cap->session == connection.workbench()->session());
	CHECK(cap->session == peer.profile->capability().session &&
	      cap->fingerprint == descriptor.fingerprint());
	unsigned batches = 0, reads = 0;
	bool batchOk = false, readOk = false;
	const std::array<std::uint32_t, 1> ids{0};
	CHECK(connection.readMany(ids, [&](const rm::ReadManyResult& result) noexcept {
		++batches;
		batchOk = result.operation.succeeded() && result.count == 1 && result.items[0].hasValue();
		if (batchOk) {
			const auto value = result.items[0].value.get<std::uint32_t>();
			batchOk = value && value.value == 42;
		}
		connection.process(); // A callback cannot recursively advance the owner loop.
	}));
	CHECK(connection.readFieldValue(
	    0, [&](const telemetry::integration::dynamic::DynamicResult& result) noexcept {
		    ++reads;
		    const auto value = result.value.get<std::uint32_t>();
		    readOk =
		        result.operation.succeeded() && result.hasValue() && value && value.value == 42;
	    }));
	CHECK(device.reads == before); // Both requests enqueue without implicit process().
	CHECK(await([&] {
		return batches == 1 && reads == 1;
	}));
	CHECK(batchOk && readOk && peer.encoded == 2 && device.reads == before + 2);
	const auto session = connection.workbench()->session();
	connection.disconnect();
	CHECK(await([&] {
		return !peer.socket;
	}));
	CHECK(!connection.ready() && !connection.readManySupported() &&
	      !connection.readManyCapability());
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection.ready();
	}));
	CHECK(connection.workbench()->session() > session &&
	      peer.profile->capability().session > session);
	unsigned cancelled = 0;
	CHECK(connection.readMany(ids, [&](const rm::ReadManyResult& result) noexcept {
		++cancelled;
		CHECK(result.operation.status == cl::Status::Closed ||
		      result.operation.status == cl::Status::Cancelled);
	}));
	connection.disconnect(); // Queued callback owns its backing through synchronous retirement.
	CHECK(cancelled == 1 && !connection.ready());
	CHECK(device.reads == before + 2);
}

void discoveryFailures()
{
	const auto before = device.reads;
	for (const auto mode :
	     {Mode::MissingCapability, Mode::MissingSession, Mode::WrongFingerprint}) {
		Peer peer(mode);
		qt::ConnectionSession connection;
		CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
		CHECK(await([&] {
			return !connection.connecting();
		}));
		// A device whose capability files do not match connects files only.
		CHECK(!connection.ready() && !connection.readManySupported() && connection.filesOnly() &&
		      connection.limitation() == qt::ConnectionSession::Limitation::Capability &&
		      connection.lastError().isEmpty() && !connection.limitationText().isEmpty());
		CHECK(peer.encoded == 0 && device.reads == before);
	}
	Peer peer(Mode::Legacy);
	qt::ConnectionSession connection;
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection.ready();
	}));
	CHECK(!connection.readManySupported() && !connection.readManyCapability());
	unsigned callbacks = 0;
	const std::array<std::uint32_t, 1> ids{0};
	const auto rejected = connection.readMany(ids, [&](const rm::ReadManyResult&) noexcept {
		++callbacks;
	});
	CHECK(!rejected && rejected.status == cl::Status::Unsupported && callbacks == 0);
	CHECK(device.reads == before && peer.encoded == 0);
	CHECK(connection.readFieldValue(
	    0, [&](const telemetry::integration::dynamic::DynamicResult& result) noexcept {
		    ++callbacks;
		    CHECK(result.operation.succeeded() && result.hasValue());
	    }));
	CHECK(await([&] {
		return callbacks == 1;
	}));
	CHECK(device.reads == before + 1);
}

void lifetime()
{
	Peer peer;
	auto connection = std::make_unique<qt::ConnectionSession>();
	CHECK(connection->connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection->ready();
	}));
	unsigned callbacks = 0;
	bool validAfterOwnerDestruction = false;
	const std::array<std::uint32_t, 1> ids{0};
	CHECK(connection->readMany(ids, [&](const rm::ReadManyResult& result) noexcept {
		++callbacks;
		connection.reset();
		if (result.count == 1) {
			const auto value = result.items[0].value.get<std::uint32_t>();
			validAfterOwnerDestruction = value && value.value == 42;
		}
	}));
	CHECK(await([&] {
		return callbacks == 1;
	}));
	CHECK(!connection && validAfterOwnerDestruction);
	CHECK(await([&] {
		return !peer.socket;
	}));
}
} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	modern();
	discoveryFailures();
	lifetime();
	std::printf("Qt ReadMany session: %u checks, 0 failures\n", checks);
}

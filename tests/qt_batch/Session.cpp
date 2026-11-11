// Public Qt Session consumer against a native localhost server, no UX. MIT.
#include <integration/qt/ConnectionSession.hpp>
#include <integration/tcp/Server.hpp>
#include <integration/tcp/EncodedProfile.hpp>
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
namespace bt = telemetry::integration::batch;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Qt Batch session check %u failed at line %d\n", checks, line);
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

struct Request {
	std::uint32_t value;
};

struct Response {
	std::uint32_t value;
};

struct Device {
	unsigned reads = 0, writes = 0, commands = 0, services = 0;
	std::uint32_t value = 42;

	std::uint32_t read() noexcept
	{
		++reads;
		return value;
	}

	telemetry::WriteResult write(std::uint32_t next) noexcept
	{
		++writes;
		value = next;
		return telemetry::WriteResult::Applied;
	}

	telemetry::CommandResult command(Request add) noexcept
	{
		++commands;
		value += add.value;
		return telemetry::CommandResult::Accepted;
	}

	telemetry::ServiceResult<Response> service(Request factor) noexcept
	{
		++services;
		return telemetry::ServiceResult<Response>::success(Response{value * factor.value});
	}
} device;

constexpr telemetry::FieldTable localFields{
    telemetry::field<&Device::read, &Device::write>("Value", device)};
constexpr telemetry::FieldCatalogTable fields{telemetry::group("test", localFields)};
constexpr telemetry::CommandTable commandTable{telemetry::command<&Device::command>("Add", device)};
constexpr telemetry::CommandCatalogTable commands{telemetry::group("control", commandTable)};
constexpr telemetry::ServiceTable serviceTable{
    telemetry::service<&Device::service>("Scale", device)};
constexpr telemetry::ServiceCatalogTable services{telemetry::group("control", serviceTable)};
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
	WrongFingerprint,
	WrongBatchFingerprint,
	WrongBatchSession,
	ReadManyOnly
};

class Peer {
public:
	QTcpServer listener;
	QPointer<QTcpSocket> socket;
	std::unique_ptr<tcp::EncodedProfile> profile;
	std::unique_ptr<tcp::ReadManyProfile> readOnlyProfile;
	std::unique_ptr<tcp::Server> server;
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 8192> rx{}, tx{};
	QByteArray pending;
	unsigned connections = 0, encoded = 0;
	bool dropBatch = false;
	Mode selectedMode;

	explicit Peer(Mode mode = Mode::Modern) : selectedMode(mode)
	{
		CHECK(listener.listen(QHostAddress::LocalHost, 0));
		QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this, mode] {
			auto* candidate = listener.nextPendingConnection();
			CHECK(candidate && !socket);
			socket = candidate;
			++connections;
			if (mode == Mode::Modern || mode == Mode::WrongFingerprint ||
			    mode == Mode::WrongBatchFingerprint || mode == Mode::WrongBatchSession) {
				profile = std::make_unique<tcp::EncodedProfile>(
				    packed, descriptor.fingerprint() ^ (mode == Mode::WrongFingerprint ? 1u : 0u));
				server = std::make_unique<tcp::Server>(model.view(), workspace, *profile, rx, tx);
			} else if (mode == Mode::ReadManyOnly) {
				readOnlyProfile =
				    std::make_unique<tcp::ReadManyProfile>(packed, descriptor.fingerprint());
				server = std::make_unique<tcp::Server>(model.view(), workspace, *readOnlyProfile,
				                                       rx, tx);
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
				readOnlyProfile.reset();
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
			if (!bytes.empty() && bytes[0] == std::byte{'T'}) {
				++encoded;
				const auto header = telemetry::protocol::encoded::parse(bytes);
				if (dropBatch && header &&
				    header.header.operation == telemetry::protocol::encoded::Operation::Batch) {
					server->releaseReply();
					continue;
				}
			}
			QByteArray copied{reinterpret_cast<const char*>(bytes.data()),
			                  static_cast<qsizetype>(bytes.size())};
			const auto marker = copied.indexOf("TBM1");
			if (marker >= 0 && selectedMode == Mode::WrongBatchFingerprint)
				copied[marker + 16] ^= 1;
			if (marker >= 0 && selectedMode == Mode::WrongBatchSession)
				copied[marker + 12] ^= 1;
			CHECK(socket->write(copied) == copied.size());
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
	CHECK(cap->session == peer.profile->readManyCapability().session &&
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
	      peer.profile->readManyCapability().session > session);
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
	for (const auto mode : {Mode::MissingCapability, Mode::MissingSession, Mode::WrongFingerprint,
	                        Mode::WrongBatchFingerprint, Mode::WrongBatchSession}) {
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

std::array<std::byte, 4> scalar(std::uint32_t value)
{
	return {std::byte(value & 255), std::byte((value >> 8) & 255), std::byte((value >> 16) & 255),
	        std::byte(value >> 24)};
}

void mixed()
{
	device = {};
	Peer peer;
	qt::ConnectionSession connection;
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection.ready();
	}));
	CHECK(connection.batchSupported() && connection.batchCapability());
	CHECK(connection.batchCapability()->session == connection.readManyCapability()->session);
	CHECK(device.writes == 0 && device.commands == 0 && device.services == 0);
	auto* workbench = connection.workbench();
	const auto field = workbench->bindField(0);
	unsigned reads = 0;
	CHECK(field && workbench->readField(field.endpoint, [&](const auto& result) noexcept {
		CHECK(result.published);
		++reads;
	}));
	CHECK(await([&] {
		return reads == 1;
	}));
	CHECK(workbench->beginEdit() == telemetry::runtime::Status::Ok);
	const auto oldRoot = workbench->root().value;
	CHECK(workbench->setScalar(oldRoot, std::uint32_t{55}) == telemetry::runtime::Status::Ok);
	std::array<std::byte, 4> captured{};
	CHECK(workbench->copyDraft(captured) && captured == scalar(55));
	CHECK(workbench->editing() && workbench->dirty());
	unsigned invalidated = 0;
	QObject::connect(&connection, &qt::ConnectionSession::dataInvalidated, &connection, [&] {
		++invalidated;
	});
	unsigned completed = 0;
	const auto ten = scalar(10), three = scalar(3), two = scalar(2);
	const std::array<qt::ConnectionSession::BatchRequest, 4> requests{
	    {{cl::Operation::FieldWrite, 0, ten},
	     {cl::Operation::Command, 0, three},
	     {cl::Operation::Service, 0, two},
	     {cl::Operation::FieldRead, 0, {}}}};
	auto invalid = requests;
	invalid[2].payload = cl::Input{two}.first(1);
	const auto rejected = connection.batch(invalid, [&](const bt::BatchResult&) noexcept {
		++completed;
	});
	CHECK(!rejected && rejected.status == cl::Status::InvalidArgument);
	CHECK(invalidated == 0 && completed == 0 && workbench->editing() && workbench->dirty());
	CHECK(workbench->info(oldRoot) && device.writes == 0 && !connection.busy());
	CHECK(connection.batch(requests, [&](const bt::BatchResult& result) noexcept {
		++completed;
		CHECK(result.operation.succeeded() && result.count == 4);
		CHECK(result.items[0].operation.succeeded());
		CHECK(result.items[1].operation.endpointStatus ==
		      static_cast<std::uint8_t>(telemetry::CommandResult::Accepted));
		CHECK(result.items[2].hasValue() &&
		      result.items[2].value.member("value").value.get<std::uint32_t>().value == 26);
		CHECK(result.items[3].hasValue() && result.items[3].value.get<std::uint32_t>().value == 13);
		connection.process(); // Owner loop cannot recurse from completion.
	}));
	CHECK(invalidated == 1 && !workbench->editing() && !workbench->info(oldRoot));
	CHECK(device.writes == 0 && device.commands == 0 &&
	      device.services == 0); // No implicit process.
	CHECK(await([&] {
		return completed == 1;
	}));
	CHECK(device.value == 13 && device.writes == 1 && device.commands == 1 && device.services == 1);
	connection.disconnect();
	CHECK(await([&] {
		return !peer.socket;
	}));
	CHECK(!connection.batchSupported() && !connection.batchCapability());
	CHECK(connection
	          .batch(requests,
	                 [](const auto&) noexcept {
	                 })
	          .status == cl::Status::NotAdmitted);
	connection.close();
	CHECK(connection
	          .batch(requests,
	                 [](const auto&) noexcept {
	                 })
	          .status == cl::Status::Closed);
}

void retirementDuringPublication()
{
	device = {};
	Peer peer;
	auto connection = std::make_unique<qt::ConnectionSession>();
	CHECK(connection->connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection->ready();
	}));
	const auto data = scalar(77);
	const std::array<qt::ConnectionSession::BatchRequest, 1> requests{
	    {{cl::Operation::FieldWrite, 0, data}}};
	unsigned callbacks = 0;
	cl::Status outcome = cl::Status::Ok;
	QObject::connect(connection.get(), &qt::ConnectionSession::dataInvalidated, connection.get(),
	                 [&] {
		                 connection.reset();
	                 });
	auto* raw = connection.get();
	const auto submitted = raw->batch(requests, [&](const auto& result) noexcept {
		++callbacks;
		outcome = result.operation.status;
	});
	CHECK(submitted && !connection && callbacks == 1);
	CHECK(outcome == cl::Status::Closed || outcome == cl::Status::Cancelled);
	CHECK(device.writes == 0 && await([&] {
		      return !peer.socket;
	      }));
}

struct MoveDisconnect {
	qt::ConnectionSession* owner;
	bool* armed;
	unsigned* callbacks;
	cl::Status* status;

	MoveDisconnect(qt::ConnectionSession& session, bool& flag, unsigned& count,
	               cl::Status& result) noexcept
	    : owner(&session), armed(&flag), callbacks(&count), status(&result)
	{}

	MoveDisconnect(MoveDisconnect&& other) noexcept
	    : owner(other.owner), armed(other.armed), callbacks(other.callbacks), status(other.status)
	{
		if (*armed) {
			*armed = false;
			owner->disconnect();
		}
	}

	void operator()(const bt::BatchResult& result) noexcept
	{
		++*callbacks;
		*status = result.operation.status;
	}
};

void callbackMoveDisconnect()
{
	device = {};
	Peer peer;
	qt::ConnectionSession connection;
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection.ready();
	}));
	const auto data = scalar(66);
	const std::array<qt::ConnectionSession::BatchRequest, 1> requests{
	    {{cl::Operation::FieldWrite, 0, data}}};
	bool armed = true;
	unsigned callbacks = 0;
	cl::Status outcome = cl::Status::Ok;
	CHECK(connection.batch(requests, MoveDisconnect{connection, armed, callbacks, outcome}));
	CHECK(callbacks == 1 && !connection.ready() && !connection.busy() && device.writes == 0);
	CHECK(outcome == cl::Status::Closed || outcome == cl::Status::Cancelled);
	CHECK(await([&] {
		return !peer.socket;
	}));
}

void readManyOnly()
{
	device = {};
	Peer peer{Mode::ReadManyOnly};
	qt::ConnectionSession connection;
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection.ready();
	}));
	CHECK(connection.readManySupported() && !connection.batchSupported() &&
	      !connection.batchCapability());
	const std::array<qt::ConnectionSession::BatchRequest, 1> requests{
	    {{cl::Operation::FieldRead, 0, {}}}};
	unsigned callbacks = 0;
	CHECK(connection
	          .batch(requests,
	                 [&](const auto&) noexcept {
		                 ++callbacks;
	                 })
	          .status == cl::Status::Unsupported);
	CHECK(callbacks == 0 && !connection.busy() && peer.encoded == 0 && device.reads == 0);
}

void droppedMutation()
{
	device = {};
	Peer peer;
	qt::ConnectionSession connection;
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection.ready();
	}));
	peer.dropBatch = true;
	const auto payload = scalar(77);
	const std::array<qt::ConnectionSession::BatchRequest, 1> requests{
	    {{cl::Operation::FieldWrite, 0, payload}}};
	unsigned callbacks = 0;
	cl::Status outcome = cl::Status::Ok;
	CHECK(connection.batch(requests, [&](const bt::BatchResult& result) noexcept {
		++callbacks;
		outcome = result.operation.status;
	}));
	CHECK(await([&] {
		return device.writes == 1;
	})); // Server executed after authorized handoff.
	connection.disconnect();
	CHECK(callbacks == 1 && outcome == cl::Status::OutcomeUnknown && connection.recoveryRequired());
	CHECK(connection
	          .batch(requests,
	                 [](const auto&) noexcept {
	                 })
	          .status == cl::Status::RecoveryRequired);
	CHECK(await([&] {
		return !peer.socket;
	}));
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(await([&] {
		return connection.ready();
	}));
	for (unsigned i = 0; i < 5; ++i)
		QCoreApplication::processEvents();
	CHECK(device.writes == 1 && callbacks == 1 && !connection.recoveryRequired()); // No replay.
	connection.close();
	CHECK(connection
	          .batch(requests,
	                 [](const auto&) noexcept {
	                 })
	          .status == cl::Status::Closed);
}

} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	modern();
	discoveryFailures();
	lifetime();
	mixed();
	readManyOnly();
	droppedMutation();
	retirementDuringPublication();
	callbackMoveDisconnect();
	std::printf("Qt Batch session: %u checks, 0 failures\n", checks);
}

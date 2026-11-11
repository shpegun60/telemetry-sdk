// Resource List, Stat and Read after discovery, interleaved with Encoded
// requests on one TCP stream: the Encoded backend's Resource family over the
// real TCP transport and the native server, without the client queue. MIT.
#include <integration/encoded/Backend.hpp>
#include <integration/qt/TcpTransport.hpp>
#include <integration/tcp/Server.hpp>
#include <resource/BytesFile.hpp>
#include <resource/protocol/Packets.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace qt = telemetry::integration::qt;
namespace tcp = telemetry::integration::tcp;
namespace cl = telemetry::client;
namespace rc = resource::protocol::client;
using Op = resource::protocol::Op;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Resource exchange check %u failed at line %d\n", checks, line);
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

// Files of the sizes that matter: empty, one byte, and several server pieces.
std::array<std::byte, 0> emptyBytes{};
std::array<std::byte, 1> oneBytes{std::byte{0x7e}};
std::array<std::byte, 70001> largeBytes{};
const resource::BytesFile descriptorFile{packed};
const resource::BytesFile emptyFile{emptyBytes};
const resource::BytesFile oneFile{oneBytes};
const resource::BytesFile largeFile{largeBytes};
const auto files = resource::filesystem(resource::file("/telemetry/descriptor.bin", descriptorFile),
                                        resource::file("/logs/empty.csv", emptyFile),
                                        resource::file("/logs/one.bin", oneFile),
                                        resource::file("/logs/large.bin", largeFile));

class Peer {
public:
	QTcpServer listener;
	QPointer<QTcpSocket> socket;
	std::unique_ptr<tcp::Server> server;
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 8192> rx{}, tx{};
	QByteArray pending;

	Peer()
	{
		CHECK(listener.listen(QHostAddress::LocalHost, 0));
		QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this] {
			auto* candidate = listener.nextPendingConnection();
			socket = candidate;
			server = std::make_unique<tcp::Server>(model.view(), workspace, files.view(), rx, tx);
			QObject::connect(candidate, &QTcpSocket::readyRead, &listener, [this] {
				read();
			});
		});
	}

	~Peer()
	{
		if (socket) {
			QObject::disconnect(socket, nullptr, &listener, nullptr);
			socket->abort();
		}
	}

	void read()
	{
		pending += socket->readAll();
		while (!pending.isEmpty()) {
			const auto result =
			    server->receive({reinterpret_cast<const std::byte*>(pending.constData()),
			                     static_cast<std::size_t>(pending.size())});
			pending.remove(0, static_cast<qsizetype>(result.consumed));
			if (result.status == tcp::Status::NeedMore)
				break;
			CHECK(result.status == tcp::Status::Ready);
			const auto bytes = server->reply();
			socket->write(reinterpret_cast<const char*>(bytes.data()),
			              static_cast<qint64>(bytes.size()));
			server->releaseReply();
		}
	}
};

QElapsedTimer clockSource;

std::uint32_t now(void*) noexcept
{
	return static_cast<std::uint32_t>(clockSource.elapsed());
}

// The owner loop of a port: stage what the backend offers, hand it off, feed
// replies back. One exchange at a time, as ConnectionStorage does it.
class Owner {
public:
	qt::TcpTransport transport;
	std::array<std::byte, 65568> txStorage{}, rxStorage{};
	cl::Clock clock{nullptr, &now};
	telemetry::integration::encoded::EncodedBackend backend{txStorage, rxStorage, clock, 5000,
	                                                        false,     false,     true};
	bool discovered = false;
	QString error;

	Owner()
	{
		QObject::connect(&transport, &qt::DeviceTransport::descriptorReceived, &transport,
		                 [this](const QByteArray&) {
			                 discovered = true;
		                 });
		QObject::connect(&transport, &qt::DeviceTransport::packetAccepted, &transport,
		                 [this](quint32 session, quint32 id) {
			                 (void)backend.onSendAccepted(session, id);
		                 });
		QObject::connect(&transport, &qt::DeviceTransport::packetReceived, &transport,
		                 [this](const QByteArray& bytes, quint32 session, quint32 id) {
			                 (void)backend.onReply(
			                     {reinterpret_cast<const std::byte*>(bytes.constData()),
			                      static_cast<std::size_t>(bytes.size())},
			                     session, id);
		                 });
		QObject::connect(&transport, &qt::DeviceTransport::transportError, &transport,
		                 [this](const QString& text) {
			                 error = text;
		                 });
	}

	void pump()
	{
		backend.process(now(nullptr));
		const auto packet = backend.nextPacket();
		if (packet && transport.canStage() && transport.stage(packet)) {
			if (backend.prepareHandoff(packet.session, packet.requestId))
				CHECK(transport.sendStaged());
			else
				transport.discardStaged();
		}
	}

	// Runs one request to completion and hands back a copy of its result.
	std::optional<std::vector<std::byte>> run(cl::RequestView request, cl::Status& status)
	{
		status = backend.start(request, now(nullptr));
		if (status != cl::Status::Ok)
			return std::nullopt;
		CHECK(await([&] {
			pump();
			return backend.completed();
		}));
		const auto result = backend.result();
		status = result.status;
		std::optional<std::vector<std::byte>> bytes;
		if (result.status == cl::Status::Ok)
			bytes.emplace(result.payload.begin(), result.payload.end());
		backend.releaseResult();
		return bytes;
	}

	std::vector<std::byte> resource(std::span<const std::byte> request)
	{
		cl::Status status{};
		const auto bytes =
		    run({cl::Operation::Resource, 0, request,
		         static_cast<std::uint32_t>(resource::protocol::wire::chunkReplyHeaderSize +
		                                    resource::protocol::wire::maxPayloadSize)},
		        status);
		CHECK(status == cl::Status::Ok && bytes);
		return bytes ? *bytes : std::vector<std::byte>{};
	}

	std::uint32_t fieldRead()
	{
		cl::Status status{};
		const auto bytes = run({cl::Operation::FieldRead, 0, {}, 4}, status);
		CHECK(status == cl::Status::Ok && bytes && bytes->size() == 4);
		std::uint32_t value = 0;
		if (bytes && bytes->size() == 4)
			std::memcpy(&value, bytes->data(), 4);
		return value;
	}
};

struct Listed {
	std::vector<std::string> paths;
};

Listed list(Owner& owner)
{
	Listed listed;
	resource::Cursor cursor = 0;
	for (bool eof = false; !eof;) {
		std::array<std::byte, resource::protocol::wire::listRequestSize> request{};
		CHECK(rc::makeList(request, cursor).status == rc::BuildStatus::Ok);
		const auto reply = owner.resource(request);
		const auto parsed = rc::parseList(reply);
		CHECK(parsed && !parsed.shortError && parsed.response.status == resource::Status::Ok);
		for (const auto path : parsed.response.paths)
			listed.paths.emplace_back(path);
		cursor = parsed.response.next;
		eof = parsed.response.eof || !parsed;
	}
	return listed;
}

std::vector<std::byte> readFile(Owner& owner, resource::FileIndex index, resource::FileSize size)
{
	std::array<std::byte, resource::protocol::wire::statRequestSize> stat{};
	CHECK(rc::makeStat(stat, index).status == rc::BuildStatus::Ok);
	const auto statted = rc::parseStat(owner.resource(stat));
	CHECK(statted && statted.response.status == resource::Status::Ok &&
	      statted.response.size == size &&
	      resource::has(statted.response.flags, resource::FileFlag::Readable));
	std::vector<std::byte> bytes;
	resource::Cursor cursor = 0;
	unsigned pieces = 0;
	for (bool eof = false; !eof && pieces < 1000; ++pieces) {
		std::array<std::byte, resource::protocol::wire::readRequestSize> request{};
		CHECK(rc::makeRead(request, index, cursor).status == rc::BuildStatus::Ok);
		const auto reply = owner.resource(request);
		const auto parsed = rc::parseRead(reply);
		CHECK(parsed && !parsed.shortError && parsed.response.status == resource::Status::Ok);
		bytes.insert(bytes.end(), parsed.response.data.begin(), parsed.response.data.end());
		cursor = parsed.response.next;
		eof = parsed.response.eof;
		// An Encoded request between the pieces still finds the framer on Encoded.
		if (pieces == 1)
			CHECK(owner.fieldRead() == 42);
	}
	return bytes;
}

void refusals()
{
	std::array<std::byte, 64> tx{}, rx{};
	telemetry::integration::encoded::EncodedBackend closed{tx, rx, {nullptr, &now}, 1000};
	CHECK(!closed.supportsResource());
	std::array<std::byte, resource::protocol::wire::statRequestSize> stat{};
	CHECK(rc::makeStat(stat, 0).status == rc::BuildStatus::Ok);
	CHECK(closed.openSession(1));
	CHECK(closed.start({cl::Operation::Resource, 0, stat, 6}, now(nullptr)) ==
	      cl::Status::Unsupported);
	std::array<std::byte, 64> tx2{}, rx2{};
	telemetry::integration::encoded::EncodedBackend open{tx2,   rx2, {nullptr, &now}, 1000, false,
	                                                     false, true};
	CHECK(open.supportsResource() && open.openSession(1));
	CHECK(open.start({cl::Operation::Resource, 3, stat, 6}, now(nullptr)) ==
	      cl::Status::InvalidArgument);
	CHECK(open.start({cl::Operation::Resource, 0, {}, 6}, now(nullptr)) ==
	      cl::Status::InvalidArgument);
	std::array<std::byte, 20> write{};
	CHECK(rc::makeWrite(write, 0, 0, std::span<const std::byte>{}, true).status ==
	      rc::BuildStatus::Ok);
	CHECK(open.start({cl::Operation::Resource, 0, write, 14}, now(nullptr)) ==
	      cl::Status::Unsupported);
	CHECK(open.start({cl::Operation::Resource, 0, stat, 6}, now(nullptr)) == cl::Status::Ok);
	const auto packet = open.nextPacket();
	// The raw request, no TEQ1 header; a read-only packet that never executes.
	CHECK(packet.reply == cl::ReplyFrame::Resource && packet.resourceOperation == 2 &&
	      !packet.mayExecute);
	CHECK(packet.bytes.size() == stat.size() &&
	      std::equal(stat.begin(), stat.end(), packet.bytes.begin()));
	open.cancel(cl::Status::Cancelled);
	CHECK(open.completed() && open.result().status == cl::Status::Cancelled);
	open.releaseResult();
	// An Encoded packet keeps the defaults.
	CHECK(open.start({cl::Operation::FieldRead, 0, {}, 4}, now(nullptr)) == cl::Status::Ok);
	const auto encoded = open.nextPacket();
	CHECK(encoded.reply == cl::ReplyFrame::Encoded && encoded.resourceOperation == 0 &&
	      encoded.mayExecute);
}

void exchange()
{
	for (std::size_t i = 0; i < largeBytes.size(); ++i)
		largeBytes[i] = std::byte(i * 31 + 7);
	Peer peer;
	Owner owner;
	CHECK(owner.transport.supportsResourceReplies());
	owner.transport.connectTo(QStringLiteral("127.0.0.1"), peer.listener.serverPort());
	CHECK(await([&] {
		return owner.discovered || !owner.error.isEmpty();
	}));
	CHECK(owner.discovered && owner.transport.ready());
	CHECK(owner.backend.openSession(1));
	CHECK(owner.fieldRead() == 42); // Encoded first: the framer is on Encoded after discovery.
	const auto listed = list(owner);
	const std::vector<std::string> expected{"/telemetry/descriptor.bin", "/logs/empty.csv",
	                                        "/logs/one.bin", "/logs/large.bin"};
	CHECK(listed.paths == expected);
	const auto descriptorBytes = readFile(owner, 0, packed.size());
	CHECK(descriptorBytes.size() == packed.size() &&
	      std::equal(packed.begin(), packed.end(), descriptorBytes.begin()));
	CHECK(readFile(owner, 1, 0).empty());
	const auto one = readFile(owner, 2, 1);
	CHECK(one.size() == 1 && one[0] == std::byte{0x7e});
	const auto large = readFile(owner, 3, largeBytes.size());
	CHECK(large.size() == largeBytes.size() &&
	      std::equal(largeBytes.begin(), largeBytes.end(), large.begin()));
	// A staged Resource packet that is discarded leaves the stream as it was.
	std::array<std::byte, resource::protocol::wire::statRequestSize> stat{};
	CHECK(rc::makeStat(stat, 0).status == rc::BuildStatus::Ok);
	CHECK(owner.backend.start({cl::Operation::Resource, 0, stat, 6}, now(nullptr)) ==
	      cl::Status::Ok);
	const auto packet = owner.backend.nextPacket();
	CHECK(owner.transport.canStage() && owner.transport.stage(packet));
	owner.transport.discardStaged();
	owner.backend.cancel(cl::Status::Cancelled);
	CHECK(owner.backend.completed());
	owner.backend.releaseResult();
	CHECK(owner.fieldRead() == 42);
	CHECK(owner.error.isEmpty() && owner.transport.ready());
	owner.transport.close();
}
} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	clockSource.start();
	refusals();
	exchange();
	std::printf("Resource exchange: %u checks, 0 failures\n", checks);
	return 0;
}

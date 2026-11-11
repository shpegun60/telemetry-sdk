// The device's files through a ConnectionSession over real TCP: listing, a
// FileReader driven from the owner loop while Field reads share the queue,
// a lost connection mid-read, and refusals without a connection. MIT.
#include <integration/qt/ConnectionSession.hpp>
#include <integration/tcp/Server.hpp>
#include <resource/BytesFile.hpp>
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
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace qt = telemetry::integration::qt;
namespace tcp = telemetry::integration::tcp;
namespace files = telemetry::integration::files;
namespace cl = telemetry::client;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Device files session check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

bool await(const std::function<bool()>& done, int milliseconds = 10000)
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

std::array<std::byte, 200000> largeBytes{};
const resource::BytesFile descriptorFile{packed};
const resource::BytesFile largeFile{largeBytes};
const auto table = resource::filesystem(resource::file("/telemetry/descriptor.bin", descriptorFile),
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
			pending.clear();
			server = std::make_unique<tcp::Server>(model.view(), workspace, table.view(), rx, tx);
			QObject::connect(candidate, &QTcpSocket::readyRead, &listener, [this, candidate] {
				if (socket == candidate)
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

struct Sink {
	std::vector<std::byte>* into;

	bool operator()(cl::Input piece) noexcept
	{
		into->insert(into->end(), piece.begin(), piece.end());
		return true;
	}
};

using Reader = files::FileReader<qt::ConnectionSession, Sink>;

// The owner loop: Qt events, plus one poll of the reader per turn.
bool run(Reader& reader, const std::function<void()>& between = {})
{
	return await([&] {
		(void)reader.poll();
		if (between)
			between();
		return reader.finished();
	});
}

void refusedWithoutConnection()
{
	qt::ConnectionSession session;
	CHECK(!session.filesSupported());
	const auto submitted = files::list(session, 0, [](const files::ListResult&) noexcept {
	});
	CHECK(!submitted && submitted.status == cl::Status::NotAdmitted);
}

void reading()
{
	for (std::size_t i = 0; i < largeBytes.size(); ++i)
		largeBytes[i] = std::byte((i * 7) ^ (i >> 8));
	Peer peer;
	qt::ConnectionSession session;
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	CHECK(await([&] {
		return session.ready() || (!session.connecting() && !session.lastError().isEmpty());
	}));
	CHECK(session.ready() && session.filesSupported());

	std::vector<std::string> paths;
	bool listed = false;
	CHECK(files::list(session, 0, [&](const files::ListResult& result) noexcept {
		CHECK(result.succeeded() && result.reply.eof);
		for (const auto path : result.reply.paths)
			paths.emplace_back(path);
		listed = true;
	}));
	CHECK(await([&] {
		return listed;
	}));
	CHECK((paths == std::vector<std::string>{"/telemetry/descriptor.bin", "/logs/large.bin"}));

	std::vector<std::byte> descriptorBytes;
	Reader descriptorReader{session, Sink{&descriptorBytes}};
	CHECK(descriptorReader.start(0) == cl::Status::Ok);
	CHECK(run(descriptorReader));
	CHECK(descriptorReader.state() == files::ReadState::Complete &&
	      descriptorBytes.size() == packed.size() &&
	      std::equal(packed.begin(), packed.end(), descriptorBytes.begin()));

	// A large file in many pieces, with Field reads queued between them.
	std::vector<std::byte> large;
	Reader largeReader{session, Sink{&large}};
	unsigned fieldReads = 0, fieldOk = 0;
	CHECK(largeReader.start(1) == cl::Status::Ok);
	CHECK(run(largeReader, [&] {
		if (fieldReads < 5 && largeReader.received() > fieldReads * 30000) {
			if (session.readFieldValue(
			        0, [&](const telemetry::integration::dynamic::DynamicResult& result) noexcept {
				        const auto value = result.value.get<std::uint32_t>();
				        fieldOk += result.operation.succeeded() && value && value.value == 42;
			        }))
				++fieldReads;
		}
	}));
	CHECK(largeReader.state() == files::ReadState::Complete &&
	      largeReader.size() == largeBytes.size() && large.size() == largeBytes.size() &&
	      std::equal(largeBytes.begin(), largeBytes.end(), large.begin()));
	CHECK(await([&] {
		return fieldOk == fieldReads;
	}));
	CHECK(fieldReads >= 3 && fieldOk == fieldReads);

	// Cancelled from the owner loop after a few pieces: it stops short.
	std::vector<std::byte> partial;
	Reader cancelled{session, Sink{&partial}};
	CHECK(cancelled.start(1) == cl::Status::Ok);
	CHECK(run(cancelled, [&] {
		if (cancelled.received() > 10000)
			cancelled.cancel();
	}));
	CHECK(cancelled.state() == files::ReadState::Cancelled && partial.size() < largeBytes.size() &&
	      session.ready());

	// The connection goes away in the middle of a read.
	std::vector<std::byte> cut;
	Reader cutReader{session, Sink{&cut}};
	CHECK(cutReader.start(1) == cl::Status::Ok);
	CHECK(run(cutReader, [&] {
		if (cutReader.received() > 20000 && peer.socket)
			peer.socket->abort();
	}));
	// Closed, lost or cancelled by the retiring queue - never complete.
	CHECK((cutReader.state() == files::ReadState::Failed ||
	       cutReader.state() == files::ReadState::Cancelled) &&
	      cut.size() < largeBytes.size());
	CHECK(await([&] {
		return !session.ready();
	}));
	CHECK(!session.filesSupported());
}
} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	refusedWithoutConnection();
	reading();
	std::printf("Device files session: %u checks, 0 failures\n", checks);
	return 0;
}

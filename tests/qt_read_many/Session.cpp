// Public Qt Session consumer against a native localhost server, no UX. MIT.
#include <integration/qt/ConnectionSession.hpp>
#include <integration/tcp/Server.hpp>
#include <resource/BytesFile.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QPointer>
#include <QThread>
#include <algorithm>
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

// Runs the owner loop, without sleeping, until done or 5 s: the time taken
// is the session's own, not a polling loop's.
qint64 runUntil(const std::function<bool()>& done)
{
	QElapsedTimer clock;
	clock.start();
	QEventLoop loop;
	QTimer poll, limit;
	QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
		if (done())
			loop.quit();
	});
	QObject::connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
	poll.start(0);
	limit.setSingleShot(true);
	limit.start(5000);
	if (!done())
		loop.exec();
	return done() ? clock.elapsed() : -1;
}

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
	// Takes requests in and never answers them.
	bool silent = false;

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
		if (silent)
			return;
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
	// An accepted request is written on the owner loop's next turn, not on
	// the session timer's next 5 ms tick. 64 ReadMany, each submitted once the
	// one before has been answered, take far less than 64 ticks (320 ms).
	{
		int chained = 0;
		bool chainOk = true;
		std::function<void()> next = [&] {
			chainOk = chainOk &&
			          bool(connection.readMany(ids, [&](const rm::ReadManyResult& result) noexcept {
				          chainOk = chainOk && result.operation.succeeded();
				          // A callback cannot submit; the next goes on the next turn.
				          if (++chained < 64)
					          QTimer::singleShot(0, &connection, [&] {
						          next();
					          });
			          }));
		};
		next();
		const auto ms = runUntil([&] {
			return chained == 64;
		});
		std::printf("Qt ReadMany chained: 64 exchanges in %lld ms\n", static_cast<long long>(ms));
		CHECK(chainOk && ms >= 0 && ms < 200);
		// The same for single Field reads.
		int fields = 0;
		bool fieldsOk = true;
		std::function<void()> field = [&] {
			fieldsOk =
			    fieldsOk &&
			    bool(connection.readFieldValue(
			        0, [&](const telemetry::integration::dynamic::DynamicResult& result) noexcept {
				        fieldsOk = fieldsOk && result.operation.succeeded();
				        if (++fields < 64)
					        QTimer::singleShot(0, &connection, [&] {
						        field();
					        });
			        }));
		};
		field();
		const auto fieldMs = runUntil([&] {
			return fields == 64;
		});
		std::printf("Qt FieldRead chained: 64 exchanges in %lld ms\n",
		            static_cast<long long>(fieldMs));
		CHECK(fieldsOk && fieldMs >= 0 && fieldMs < 200);
		// Requests queued together: each is sent as soon as the one before
		// it is settled, not on the next tick (7 ticks would be 35 ms).
		int queued = 0;
		bool queuedOk = true;
		for (int i = 0; i < 8; ++i)
			queuedOk =
			    queuedOk &&
			    bool(connection.readMany(ids, [&](const rm::ReadManyResult& result) noexcept {
				    queuedOk = queuedOk && result.operation.succeeded();
				    ++queued;
			    }));
		const auto queuedMs = runUntil([&] {
			return queued == 8;
		});
		std::printf("Qt ReadMany queued: 8 exchanges in %lld ms\n",
		            static_cast<long long>(queuedMs));
		CHECK(queuedOk && queuedMs >= 0 && queuedMs < 30);
	}
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
	// The two first reads, then the 64 + 64 + 8 timed ones.
	CHECK(device.reads == before + 2 + 136);
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

// The session runs only when there is work. Idle it publishes nothing. A
// request submitted to the Workbench past it is written at once after wake(),
// and found by the idle heartbeat without it. Behind an unanswered request,
// waiting publishes nothing, a request that times out completes within the
// 100 ms backstop of its deadline, and a cancel reports on the next turn.
void onlyWithWork()
{
	Peer peer;
	qt::ConnectionSession connection;
	CHECK(connection.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()));
	CHECK(runUntil([&] {
		      return connection.ready();
	      }) >= 0);
	unsigned published = 0;
	QObject::connect(&connection, &qt::ConnectionSession::progressed, &connection, [&] {
		++published;
	});
	// Nothing pending: no pass publishes, and no timer runs one but the
	// heartbeat. A 5 ms timer published on every tick, 31 to 100 in 500 ms.
	QElapsedTimer idle;
	idle.start();
	(void)runUntil([&] {
		return idle.elapsed() >= 500;
	});
	std::printf("Qt session idle: %u progressed in 500 ms\n", published);
	CHECK(published == 0 && !connection.busy());

	// Submitted to the Workbench directly, then woken: written on the next
	// turn, far sooner than the heartbeat or the backstop would.
	auto* workbench = connection.workbench();
	CHECK(workbench);
	const auto field = workbench->bindField(0);
	CHECK(field);
	unsigned direct = 0;
	bool directOk = true;
	const auto readDirect = [&] {
		return bool(workbench->readField(
		    field.endpoint,
		    [&](const telemetry::integration::inspector::ReadResult& result) noexcept {
			    directOk = directOk && result.operation.succeeded() && result.published;
			    ++direct;
		    }));
	};
	QElapsedTimer clock;
	clock.start();
	CHECK(readDirect());
	connection.wake();
	CHECK(runUntil([&] {
		      return direct == 1;
	      }) >= 0);
	const auto wokenMs = clock.elapsed();
	std::printf("Qt session woken: a direct Workbench read in %lld ms\n",
	            static_cast<long long>(wokenMs));
	CHECK(directOk && wokenMs < 90);
	// Not woken: the idle heartbeat finds it within its second.
	clock.restart();
	CHECK(readDirect());
	CHECK(runUntil([&] {
		      return direct == 2;
	      }) >= 0);
	const auto heartbeatMs = clock.elapsed();
	std::printf("Qt session not woken: a direct Workbench read in %lld ms\n",
	            static_cast<long long>(heartbeatMs));
	CHECK(directOk && heartbeatMs < 1300);

	// An unanswered request, sent: waiting for it publishes nothing.
	peer.silent = true;
	const std::array<std::uint32_t, 1> ids{0};
	unsigned unanswered = 0;
	CHECK(connection.readMany(ids, [&](const rm::ReadManyResult&) noexcept {
		++unanswered;
	}));
	CHECK(runUntil([&] {
		      return !peer.pending.isEmpty();
	      }) >= 0);
	// The pass that sent it has published; from here on only waiting is counted.
	published = 0;
	clock.restart();
	(void)runUntil([&] {
		return clock.elapsed() >= 250;
	});
	CHECK(published == 0 && unanswered == 0 && connection.busy());
	// Queued behind it with a 200 ms deadline: it times out within the
	// backstop of that deadline, and nothing is published before it does.
	bool timedOut = false;
	unsigned publishedBefore = 1;
	clock.restart();
	CHECK(connection.readMany(
	    ids,
	    [&](const rm::ReadManyResult& result) noexcept {
		    timedOut = result.operation.status == cl::Status::Timeout;
		    publishedBefore = published;
	    },
	    200));
	CHECK(runUntil([&] {
		      return timedOut;
	      }) >= 0);
	const auto timeoutMs = clock.elapsed();
	std::printf("Qt session: a 200 ms deadline behind an unanswered request ended in %lld ms\n",
	            static_cast<long long>(timeoutMs));
	CHECK(timedOut && publishedBefore == 0 && timeoutMs >= 195 && timeoutMs < 200 + 100 + 80);
	CHECK(runUntil([&] {
		      return published == 1;
	      }) >= 0);
	// Six queued behind it, left until their submissions' pass has run, then
	// cancelled together. The cancel queues a pass. A pass calls process() up
	// to three times and each call completes at most one request, so the
	// completions queue further passes: all six report Cancelled on
	// consecutive turns, not on the backstop's next tick. Three rounds, so a
	// tick that happens to fall just after the cancels cannot pass for this.
	qint64 slowestCancelMs = 0;
	for (int round = 0; round < 3; ++round) {
		unsigned cancelled = 0;
		std::array<cl::Submission, 6> queued{};
		for (auto& submission : queued) {
			submission = connection.readMany(ids, [&](const rm::ReadManyResult& result) noexcept {
				if (result.operation.status == cl::Status::Cancelled)
					++cancelled;
			});
			CHECK(submission);
		}
		clock.restart();
		(void)runUntil([&] {
			return clock.elapsed() >= 30;
		});
		CHECK(cancelled == 0);
		clock.restart();
		for (const auto& submission : queued)
			CHECK(connection.cancel(submission.handle));
		CHECK(runUntil([&] {
			      return cancelled == queued.size();
		      }) >= 0);
		slowestCancelMs = std::max(slowestCancelMs, clock.elapsed());
		CHECK(cancelled == queued.size());
	}
	std::printf("Qt session: six cancels reported in %lld ms at most, in 3 rounds\n",
	            static_cast<long long>(slowestCancelMs));
	CHECK(slowestCancelMs < 20 && unanswered == 0);
	connection.disconnect();
	CHECK(unanswered == 1 && !connection.ready());
}
} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	modern();
	discoveryFailures();
	lifetime();
	onlyWithWork();
	std::printf("Qt ReadMany session: %u checks, 0 failures\n", checks);
}

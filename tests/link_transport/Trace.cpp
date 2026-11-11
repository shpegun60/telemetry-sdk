// What a ConnectionSession over the TCP transport does, as an ordered trace of
// its observable state, for a fixed set of peer behaviours. The trace is
// compared with a golden recorded on the revision before the transport was
// split into a link and a framer, so the split proves it changed nothing. MIT.
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
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
namespace qt = telemetry::integration::qt;
namespace tcp = telemetry::integration::tcp;
namespace rm = telemetry::integration::read_many;

namespace {
bool await(const std::function<bool()>& done, int milliseconds = 15000)
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

// What the peer does with the first Encoded request after discovery.
enum class Behaviour {
	Reply,           // answer every request
	CloseAfterReply, // answer, then close the connection at once
	Garbage,         // answer with bytes that are not a frame
	Silent           // never answer
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
	Behaviour behaviour;

	explicit Peer(Behaviour mode) : behaviour(mode)
	{
		if (!listener.listen(QHostAddress::LocalHost, 0))
			std::exit(2);
		QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this] {
			auto* candidate = listener.nextPendingConnection();
			socket = candidate;
			profile = std::make_unique<tcp::ReadManyProfile>(packed, descriptor.fingerprint());
			server = std::make_unique<tcp::Server>(model.view(), workspace, *profile, rx, tx);
			QObject::connect(candidate, &QTcpSocket::readyRead, &listener, [this] {
				read();
			});
			QObject::connect(candidate, &QTcpSocket::disconnected, &listener, [this, candidate] {
				if (socket != candidate)
					return;
				if (server)
					server->close();
				server.reset();
				profile.reset();
				pending.clear();
				socket.clear();
				candidate->deleteLater();
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

	quint16 port() const
	{
		return listener.serverPort();
	}

	void read()
	{
		if (!socket || !server)
			return;
		pending += socket->readAll();
		while (!pending.isEmpty() && server) {
			const auto result =
			    server->receive({reinterpret_cast<const std::byte*>(pending.constData()),
			                     static_cast<std::size_t>(pending.size())});
			pending.remove(0, static_cast<qsizetype>(result.consumed));
			if (result.status == tcp::Status::NeedMore)
				break;
			const auto bytes = server->reply();
			const bool encoded = !bytes.empty() && bytes[0] == std::byte{'T'};
			if (encoded && behaviour == Behaviour::Silent) {
				server->releaseReply();
				continue;
			}
			if (encoded && behaviour == Behaviour::Garbage) {
				server->releaseReply();
				socket->write(QByteArray(48, '\x5a'));
				return;
			}
			socket->write(reinterpret_cast<const char*>(bytes.data()),
			              static_cast<qint64>(bytes.size()));
			server->releaseReply();
			if (encoded && behaviour == Behaviour::CloseAfterReply) {
				socket->flush();
				socket->disconnectFromHost();
				return;
			}
		}
	}
};

// The session's observable state, written whenever it changes.
class Trace {
public:
	Trace(const char* scenario, qt::ConnectionSession& session)
	    : scenario_(scenario), session_(session)
	{
		QObject::connect(&session, &qt::ConnectionSession::stateChanged, &session, [this] {
			snapshot();
		});
		QObject::connect(&session, &qt::ConnectionSession::admissionChanged, &session, [this] {
			snapshot();
		});
	}

	void line(const std::string& text)
	{
		std::printf("%s %s\n", scenario_, text.c_str());
		std::fflush(stdout);
	}

	void snapshot()
	{
		const auto text = std::string("state ready=") + (session_.ready() ? "1" : "0") +
		                  " connecting=" + (session_.connecting() ? "1" : "0") +
		                  " recovery=" + (session_.recoveryRequired() ? "1" : "0") +
		                  " readMany=" + (session_.readManySupported() ? "1" : "0") +
		                  " batch=" + (session_.batchSupported() ? "1" : "0") + " error=\"" +
		                  session_.lastError().toStdString() + "\"";
		if (text != last_)
			line(text);
		last_ = text;
	}

private:
	const char* scenario_;
	qt::ConnectionSession& session_;
	std::string last_;
};

const std::array<std::uint32_t, 1> ids{0};

std::string readManyOutcome(const rm::ReadManyResult& result)
{
	std::string text =
	    "readMany status=" + std::to_string(static_cast<int>(result.operation.status)) +
	    " count=" + std::to_string(result.count);
	if (result.count == 1 && result.items[0].hasValue()) {
		const auto value = result.items[0].value.get<std::uint32_t>();
		text += " value=" + (value ? std::to_string(value.value) : std::string("?"));
	}
	return text;
}

void scenario(const char* name, Behaviour behaviour, bool reconnect)
{
	Peer peer(behaviour);
	qt::ConnectionSession session;
	Trace trace(name, session);
	session.setWriteChunkBytes(1);
	// Only the silent peer is meant to time out; the others get room on a loaded runner.
	session.setExchangeTimeout(behaviour == Behaviour::Silent ? 2000 : 10000);
	trace.line(std::string("connect accepted=") +
	           (session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()) ? "1" : "0"));
	await([&] {
		return session.ready() || (!session.connecting() && !session.lastError().isEmpty());
	});
	trace.snapshot();
	bool done = false;
	const auto submitted = session.readMany(ids, [&](const rm::ReadManyResult& result) noexcept {
		trace.line(readManyOutcome(result));
		done = true;
	});
	trace.line(std::string("readMany submitted=") + (submitted ? "1" : "0"));
	if (submitted)
		await([&] {
			return done;
		});
	await(
	    [&] {
		    return !session.connecting();
	    },
	    2000);
	trace.snapshot();
	if (reconnect) {
		session.disconnect();
		await([&] {
			return !peer.socket;
		});
		trace.snapshot();
		trace.line(
		    std::string("reconnect accepted=") +
		    (session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.port()) ? "1" : "0"));
		await([&] {
			return session.ready();
		});
		trace.snapshot();
	}
	session.disconnect();
	trace.snapshot();
	trace.line("end");
}
} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	scenario("reply", Behaviour::Reply, true);
	scenario("close-after-reply", Behaviour::CloseAfterReply, false);
	scenario("garbage", Behaviour::Garbage, false);
	scenario("silent", Behaviour::Silent, false);
	std::printf("trace complete\n");
	return 0;
}

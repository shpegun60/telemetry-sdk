// The device's files through a ConnectionSession over real TCP: listing, a
// FileReader driven from the owner loop while Field reads share the queue,
// a lost connection mid-read, and refusals without a connection. MIT.
#include <integration/qt/ConnectionSession.hpp>
#include <integration/tcp/EncodedProfile.hpp>
#include <integration/tcp/Server.hpp>
#include <integration/telemetry/v3/IdentityFile.hpp>
#include <resource/BytesFile.hpp>
#include <resource/FileSystem.hpp>
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
	// Makes the server for a new connection; the plain table when not given.
	using Make = std::function<std::unique_ptr<tcp::Server>(Peer&)>;
	QTcpServer listener;
	QPointer<QTcpSocket> socket;
	std::unique_ptr<tcp::Server> server;
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 8192> rx{}, tx{};
	QByteArray pending;
	Make make;

	explicit Peer(Make maker = {}) : make(std::move(maker))
	{
		CHECK(listener.listen(QHostAddress::LocalHost, 0));
		QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this] {
			auto* candidate = listener.nextPendingConnection();
			socket = candidate;
			pending.clear();
			server = make ? make(*this)
			              : std::make_unique<tcp::Server>(model.view(), workspace, table.view(), rx, tx);
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
	unsigned progressSignals = 0;
	QObject::connect(&session, &qt::ConnectionSession::discoveryProgressed, &session, [&] {
		++progressSignals;
		CHECK(session.descriptorTotal() == packed.size() &&
		      session.descriptorReceived() <= session.descriptorTotal());
	});
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	CHECK(await([&] {
		return session.ready() || (!session.connecting() && !session.lastError().isEmpty());
	}));
	CHECK(session.ready() && session.filesSupported());
	// The descriptor's progress was reported while connecting, ending complete.
	CHECK(progressSignals >= 1 && session.descriptorReceived() == packed.size());

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
// A device with the full profile serves its own files after the profile's:
// the session is a telemetry one, not files only, it lists the profile's four
// files then the application's, and reads the application's byte for byte. A
// table that does not begin with the profile's files is refused.
void profileWithApplicationFiles()
{
	tcp::EncodedProfile profile(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto withFiles =
	    resource::filesystem(profile.systemFiles(), resource::file("/logs/large.bin", largeFile));
	CHECK(withFiles.view().size() == 5 && withFiles.view().path(4) == "/logs/large.bin");
	Peer peer([&](Peer& at) {
		return std::make_unique<tcp::Server>(model.view(), at.workspace, profile, withFiles.view(), at.rx, at.tx);
	});
	qt::ConnectionSession session;
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	CHECK(await([&] {
		return session.ready();
	}));
	CHECK(!session.filesOnly() && session.filesSupported());

	std::vector<std::string> paths;
	bool listed = false;
	CHECK(files::list(session, 0, [&](const files::ListResult& result) noexcept {
		if (result.succeeded())
			for (const auto path : result.reply.paths)
				paths.emplace_back(path);
		listed = true;
	}));
	CHECK(await([&] {
		return listed;
	}));
	CHECK(paths.size() == 5 && paths[0] == "/telemetry/descriptor.bin" && paths[1] == "/telemetry/session.bin" &&
	      paths[4] == "/logs/large.bin");

	std::vector<std::byte> read;
	Reader reader(session, Sink{&read});
	CHECK(reader.start(4) == cl::Status::Ok);
	CHECK(run(reader) && reader.state() == files::ReadState::Complete);
	CHECK(read.size() == largeBytes.size() && std::equal(read.begin(), read.end(), largeBytes.begin()));
	session.disconnect();

	tcp::EncodedProfile other(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto misplaced = resource::filesystem(resource::file("/logs/large.bin", largeFile));
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 1024> rx{}, tx{};
	const tcp::Server refused(model.view(), workspace, other, misplaced.view(), rx, tx);
	CHECK(refused.closed());
}

// The identity file's bytes, as the device writes them and every client reads
// them: what is refused on either side, and that a later version's extra texts
// are skipped.
void identityBytes()
{
	namespace id = telemetry::protocol::identity;
	using resource::telemetry::v3::IdentityFile;
	const auto view = [](std::span<const std::byte> bytes) {
		id::Identity out;
		return std::pair{id::parse(bytes, out), out};
	};

	IdentityFile file;
	CHECK(file.size() == 0 && view(file.bytes()).first == id::Problem::Empty);
	CHECK(!file.rename("Bench"));
	CHECK(file.set("0028003A3132510D33373935", "Bench analyzer", "PA-3", "1.4.2"));
	auto [problem, parsed] = view(file.bytes());
	CHECK(problem == id::Problem::None && parsed.id == "0028003A3132510D33373935" &&
	      parsed.name == "Bench analyzer" && parsed.model == "PA-3" && parsed.firmware == "1.4.2");
	CHECK(file.size() == id::headerBytes + 4 * 2 + 24 + 14 + 4 + 5);
	// A rename keeps the rest; Cyrillic is UTF-8 like any other name.
	CHECK(file.rename("Стенд 2") && file.name() == "Стенд 2" && file.id() == "0028003A3132510D33373935" &&
	      file.firmware() == "1.4.2");
	// Refused, and nothing changes: an id unfit for a folder name, a control
	// character, a text too long, a broken UTF-8 sequence.
	for (const auto bad : {std::string_view{}, std::string_view{".hidden"}, std::string_view{"a/b"},
	                       std::string_view{"a b"}, std::string_view{"x:1"}})
		CHECK(!file.set(bad, "name"));
	CHECK(!file.set("A1", "tab\there") && !file.set("A1", std::string(65, 'n')) &&
	      !file.set("A1", "\xC3") && !file.set("A1", "\xC0\xAF") && !file.set("A1", "\xED\xA0\x80"));
	CHECK(file.name() == "Стенд 2");
	// Read in pieces as a device's READ does.
	std::array<std::byte, 7> piece{};
	std::vector<std::byte> joined;
	resource::Cursor cursor = 0;
	for (;;) {
		const auto result = file.read(cursor, piece);
		CHECK(result.status == resource::Status::Ok);
		joined.insert(joined.end(), piece.begin(), piece.begin() + result.written);
		cursor = result.next;
		if (result.eof)
			break;
	}
	CHECK(joined.size() == file.size() && std::equal(joined.begin(), joined.end(), file.bytes().begin()));
	CHECK(file.read(file.size() + 1, piece).status == resource::Status::InvalidCursor);
	// A rename between two pieces of one read: that read stays the old bytes
	// whole, and the next read from 0 is the new name.
	const std::vector<std::byte> before(file.bytes().begin(), file.bytes().end());
	joined.clear();
	cursor = 0;
	for (bool renamed = false;;) {
		const auto result = file.read(cursor, piece);
		CHECK(result.status == resource::Status::Ok);
		joined.insert(joined.end(), piece.begin(), piece.begin() + result.written);
		cursor = result.next;
		if (!renamed) {
			CHECK(file.rename("Renamed midway"));
			renamed = true;
		}
		if (result.eof)
			break;
	}
	CHECK(joined == before && view(joined).second.name == "Стенд 2");
	std::array<std::byte, id::maxBytes> whole{};
	const auto again = file.read(0, whole);
	CHECK(again.eof && view(std::span(whole).first(again.written)).second.name == "Renamed midway");
	CHECK(file.rename("Стенд 2"));

	// The reader's refusals.
	std::vector<std::byte> bytes(file.bytes().begin(), file.bytes().end());
	auto changed = [&](std::size_t at, std::byte value) {
		auto copy = bytes;
		copy[at] = value;
		return view(copy).first;
	};
	CHECK(changed(0, std::byte{'X'}) == id::Problem::Magic);
	CHECK(changed(4, std::byte{2}) == id::Problem::Version);
	CHECK(changed(6, std::byte{1}) == id::Problem::Flags);
	CHECK(changed(8, std::byte(bytes.size() + 1)) == id::Problem::Size);
	CHECK(changed(10, std::byte{3}) == id::Problem::Texts);
	CHECK(changed(id::headerBytes + 2, std::byte{'-'}) == id::Problem::Id);
	CHECK(view(std::span(bytes).first(5)).first == id::Problem::Truncated);
	// A later version's fifth text is skipped.
	auto longer = bytes;
	longer[10] = std::byte{5};
	longer.insert(longer.end(), {std::byte{3}, std::byte{0}, std::byte{'n'}, std::byte{'e'}, std::byte{'w'}});
	longer[8] = std::byte(longer.size());
	CHECK(view(longer).first == id::Problem::None && view(longer).second.name == "Стенд 2");

	std::array<char, 24> hex{};
	const std::array<std::byte, 3> uid{std::byte{0x00}, std::byte{0x2a}, std::byte{0xff}};
	CHECK(id::hexId(uid, hex) == "002AFF");
	CHECK(id::hexId(uid, std::span(hex).first(5)).empty());
}

// The session reads the identity once a connection is ready, before anything
// asks for it: Reading the moment ready() is true, then Known. Without the
// file it is Absent; with a broken one Unreadable, with the reason, and the
// connection is as good as without it. Disconnecting forgets it.
void sessionIdentity()
{
	resource::telemetry::v3::IdentityFile identity;
	CHECK(identity.set("EMU-00A1", "Bench analyzer", "Demo", "0.1.0"));
	const auto make = [](tcp::EncodedProfile& profile, const auto& files) {
		return [&profile, &files](Peer& at) {
			return std::make_unique<tcp::Server>(model.view(), at.workspace, profile, files.view(), at.rx, at.tx);
		};
	};

	tcp::EncodedProfile profile(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto served = resource::filesystem(profile.systemFiles(),
	                                         resource::file(telemetry::protocol::identity::path, identity),
	                                         resource::file("/logs/large.bin", largeFile));
	Peer peer(make(profile, served));
	qt::ConnectionSession session;
	int announced = 0;
	QObject::connect(&session, &qt::ConnectionSession::identityChanged, &session, [&] {
		++announced;
	});
	bool readingWhenReady = false;
	QObject::connect(&session, &qt::ConnectionSession::stateChanged, &session, [&] {
		if (session.ready() && !readingWhenReady)
			readingWhenReady = session.identityState() == qt::ConnectionSession::IdentityState::Reading;
	});
	CHECK(session.identityState() == qt::ConnectionSession::IdentityState::Absent);
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	CHECK(await([&] {
		return session.identityState() == qt::ConnectionSession::IdentityState::Known;
	}));
	CHECK(readingWhenReady && session.ready() && announced >= 1);
	CHECK(session.deviceId() == QStringLiteral("EMU-00A1") && session.deviceName() == QStringLiteral("Bench analyzer") &&
	      session.deviceModel() == QStringLiteral("Demo") && session.deviceFirmware() == QStringLiteral("0.1.0") &&
	      session.identityProblem().isEmpty());
	session.disconnect();
	CHECK(session.identityState() == qt::ConnectionSession::IdentityState::Absent && session.deviceId().isEmpty());

	// Renamed on the device: the next connection reads the new name.
	CHECK(identity.rename("Стенд 2"));
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	CHECK(await([&] {
		return session.identityState() == qt::ConnectionSession::IdentityState::Known;
	}));
	CHECK(session.deviceName() == QStringLiteral("Стенд 2") && session.deviceId() == QStringLiteral("EMU-00A1"));
	session.disconnect();

	// Not served: Absent once ready, and nothing was read for it.
	tcp::EncodedProfile plain(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto without = resource::filesystem(plain.systemFiles(), resource::file("/logs/large.bin", largeFile));
	Peer bare(make(plain, without));
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), bare.listener.serverPort()));
	CHECK(await([&] {
		return session.ready();
	}));
	CHECK(session.identityState() == qt::ConnectionSession::IdentityState::Absent);
	session.disconnect();

	// Something else under its name: Unreadable, the reason, still ready.
	const std::array<std::byte, 16> junk{std::byte{'J'}, std::byte{'U'}, std::byte{'N'}, std::byte{'K'}};
	const resource::BytesFile junkFile{junk};
	tcp::EncodedProfile third(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto broken = resource::filesystem(third.systemFiles(),
	                                         resource::file(telemetry::protocol::identity::path, junkFile));
	Peer odd(make(third, broken));
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), odd.listener.serverPort()));
	CHECK(await([&] {
		return session.identityState() == qt::ConnectionSession::IdentityState::Unreadable;
	}));
	CHECK(session.ready() && session.deviceId().isEmpty() &&
	      session.identityProblem() == QStringLiteral("not an identity file"));
	session.disconnect();
}
} // namespace

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	refusedWithoutConnection();
	reading();
	profileWithApplicationFiles();
	identityBytes();
	sessionIdentity();
	std::printf("Device files session: %u checks, 0 failures\n", checks);
	return 0;
}

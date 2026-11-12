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
			server =
			    make ? make(*this)
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
		return std::make_unique<tcp::Server>(model.view(), at.workspace, profile, withFiles.view(),
		                                     at.rx, at.tx);
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
	CHECK(paths.size() == 5 && paths[0] == "/telemetry/descriptor.bin" &&
	      paths[1] == "/telemetry/session.bin" && paths[4] == "/logs/large.bin");

	std::vector<std::byte> read;
	Reader reader(session, Sink{&read});
	CHECK(reader.start(4) == cl::Status::Ok);
	CHECK(run(reader) && reader.state() == files::ReadState::Complete);
	CHECK(read.size() == largeBytes.size() &&
	      std::equal(read.begin(), read.end(), largeBytes.begin()));
	session.disconnect();

	tcp::EncodedProfile other(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto misplaced = resource::filesystem(resource::file("/logs/large.bin", largeFile));
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 1024> rx{}, tx{};
	const tcp::Server refused(model.view(), workspace, other, misplaced.view(), rx, tx);
	CHECK(refused.closed());
}

// A device's identity a client may rename, as firmware writes it: write()
// through check() and rename(), and save() that keeps the name in two steps -
// or refuses either, when told to.
// It may also shorten a name to its first bytes, answer Ok and keep the old
// one, or keep saving without ever completing.
struct KeptIdentity : resource::telemetry::v3::IdentityFile {
	unsigned taken = 0, steps = 0;
	std::size_t shortenTo = 0;
	bool refuseName = false, refuseSave = false, ignore = false, endless = false;

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input bytes,
	                                          bool final) noexcept
	{
		std::string_view name;
		if (cursor != 0 || !final || check(bytes, name) != resource::Status::Ok)
			return {resource::Status::InvalidData, cursor};
		if (refuseName)
			return {resource::Status::InternalError, cursor};
		if (shortenTo != 0 && name.size() > shortenTo)
			name = name.substr(0, shortenTo);
		if (!ignore && !rename(name))
			return {resource::Status::InvalidData, cursor};
		++taken;
		const auto size = static_cast<std::uint32_t>(bytes.size());
		return {resource::Status::Ok, size, size, true};
	}

	[[nodiscard]] resource::SaveResult save(resource::Cursor cursor) noexcept
	{
		if (refuseSave)
			return {resource::Status::InternalError, cursor};
		++steps;
		if (endless)
			return {resource::Status::Ok, cursor + 1, false};
		return cursor == 0 ? resource::SaveResult{resource::Status::Ok, 1, false}
		                   : resource::SaveResult{resource::Status::Ok, cursor, true};
	}
};

// Renamed until it restarts: write() and no save().
struct TakenIdentity : resource::telemetry::v3::IdentityFile {
	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input bytes,
	                                          bool final) noexcept
	{
		std::string_view name;
		if (cursor != 0 || !final || check(bytes, name) != resource::Status::Ok || !rename(name))
			return {resource::Status::InvalidData, cursor};
		const auto size = static_cast<std::uint32_t>(bytes.size());
		return {resource::Status::Ok, size, size, true};
	}
};

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
	CHECK(file.rename("Стенд 2") && file.name() == "Стенд 2" &&
	      file.id() == "0028003A3132510D33373935" && file.firmware() == "1.4.2");
	// Refused, and nothing changes: an id unfit for a folder name, a control
	// character, a text too long, a broken UTF-8 sequence.
	for (const auto bad : {std::string_view{}, std::string_view{".hidden"}, std::string_view{"a/b"},
	                       std::string_view{"a b"}, std::string_view{"x:1"}})
		CHECK(!file.set(bad, "name"));
	CHECK(!file.set("A1", "tab\there") && !file.set("A1", std::string(65, 'n')) &&
	      !file.set("A1", "\xC3") && !file.set("A1", "\xC0\xAF") &&
	      !file.set("A1", "\xED\xA0\x80"));
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
	CHECK(joined.size() == file.size() &&
	      std::equal(joined.begin(), joined.end(), file.bytes().begin()));
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
	longer.insert(longer.end(),
	              {std::byte{3}, std::byte{0}, std::byte{'n'}, std::byte{'e'}, std::byte{'w'}});
	longer[8] = std::byte(longer.size());
	CHECK(view(longer).first == id::Problem::None && view(longer).second.name == "Стенд 2");

	// check() tells a device's write() whether bytes only rename it: the same
	// id, model and firmware, a name an identity holds. A plain IdentityFile is
	// read-only; the device's own class is Writable, and Savable with save().
	{
		const auto encoded = [](std::string_view id, std::string_view name,
		                        std::string_view firmware) {
			std::vector<std::byte> out(id::writtenBytes);
			out.resize(id::write({id, name, "Demo", firmware}, out));
			return out;
		};
		resource::telemetry::v3::IdentityFile unset;
		std::string_view name;
		CHECK(unset.check(encoded("EMU-1", "New", "0.1.0"), name) == resource::Status::InvalidData);
		KeptIdentity kept;
		TakenIdentity taken;
		CHECK(kept.set("EMU-1", "Old", "Demo", "0.1.0") &&
		      taken.set("EMU-1", "Old", "Demo", "0.1.0"));
		const auto renamed = encoded("EMU-1", "New name", "0.1.0");
		CHECK(kept.check(renamed, name) == resource::Status::Ok && name == "New name" &&
		      kept.name() == "Old");
		CHECK(kept.check(encoded("EMU-2", "X", "0.1.0"), name) == resource::Status::InvalidData);
		CHECK(kept.check(encoded("EMU-1", "X", "9.9"), name) == resource::Status::InvalidData);
		std::vector<std::byte> otherModel(id::writtenBytes);
		otherModel.resize(id::write({"EMU-1", "X", "Other", "0.1.0"}, otherModel));
		CHECK(kept.check(otherModel, name) == resource::Status::InvalidData);
		const std::array<std::byte, 4> junk{std::byte{'J'}};
		CHECK(kept.check(junk, name) == resource::Status::InvalidData);
		const auto table =
		    resource::filesystem(resource::file(id::path, file), resource::file("/kept", kept),
		                         resource::file("/taken", taken));
		CHECK(table.stat(0).flags == resource::FileFlag::Readable &&
		      table.stat(1).flags == (resource::FileFlag::Readable | resource::FileFlag::Writable |
		                              resource::FileFlag::Savable) &&
		      table.stat(2).flags ==
		          (resource::FileFlag::Readable | resource::FileFlag::Writable) &&
		      table.write(0, 0, {}, true).status == resource::Status::NotWritable &&
		      table.save(2, 0).status == resource::Status::NotWritable);
		const auto result = table.write(1, 0, renamed, true);
		CHECK(result.status == resource::Status::Ok && result.complete &&
		      result.consumed == renamed.size() && kept.name() == "New name" &&
		      kept.id() == "EMU-1" && kept.taken == 1);
		CHECK(table.write(1, 0, std::span(renamed).first(10), false).status ==
		          resource::Status::InvalidData &&
		      table.write(1, 4, renamed, true).status == resource::Status::InvalidData &&
		      kept.taken == 1);
	}

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
	KeptIdentity identity;
	CHECK(identity.set("EMU-00A1", "Bench analyzer", "Demo", "0.1.0"));
	const auto make = [](tcp::EncodedProfile& profile, const auto& files) {
		return [&profile, &files](Peer& at) {
			return std::make_unique<tcp::Server>(model.view(), at.workspace, profile, files.view(),
			                                     at.rx, at.tx);
		};
	};

	tcp::EncodedProfile profile(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto served = resource::filesystem(
	    profile.systemFiles(), resource::file(telemetry::protocol::identity::path, identity),
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
			readingWhenReady =
			    session.identityState() == qt::ConnectionSession::IdentityState::Reading;
	});
	CHECK(session.identityState() == qt::ConnectionSession::IdentityState::Absent);
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	CHECK(await([&] {
		return session.identityState() == qt::ConnectionSession::IdentityState::Known;
	}));
	CHECK(readingWhenReady && session.ready() && announced >= 1);
	CHECK(session.deviceId() == QStringLiteral("EMU-00A1") &&
	      session.deviceName() == QStringLiteral("Bench analyzer") &&
	      session.deviceModel() == QStringLiteral("Demo") &&
	      session.deviceFirmware() == QStringLiteral("0.1.0") &&
	      session.identityProblem().isEmpty());

	// Renamed from the session: the file written whole, saved in its two steps,
	// read back, announced. While it runs it is not renamable.
	using Result = qt::ConnectionSession::RenameResult;
	CHECK(session.renamable() && session.keepsName() && session.maxNameBytes() == 64);
	CHECK(!session.rename(QString()) && !session.nameProblem(QString()).isEmpty() &&
	      !session.nameProblem(QString(65, QLatin1Char('x'))).isEmpty() &&
	      !session.nameProblem(QStringLiteral("tab\there")).isEmpty() &&
	      !session.nameProblem(QStringLiteral(" Bench B")).isEmpty() &&
	      !session.nameProblem(QStringLiteral("Bench B ")).isEmpty() &&
	      session.nameProblem(QStringLiteral("Bench B")).isEmpty() &&
	      !session.rename(QStringLiteral("Bench analyzer")));
	bool finished = false;
	auto result = Result::NotRenamed;
	QString why;
	const auto watching = QObject::connect(&session, &qt::ConnectionSession::renameFinished,
	                                       &session, [&](Result ended, const QString& problem) {
		                                       finished = true;
		                                       result = ended;
		                                       why = problem;
	                                       });
	const auto renameTo = [&](const QString& name) {
		finished = false;
		const bool started = session.rename(name);
		CHECK(started && !session.renamable() && !session.rename(QStringLiteral("Other")));
		CHECK(await([&] {
			return finished;
		}));
		return started;
	};
	CHECK(renameTo(QStringLiteral("Bench B")));
	CHECK(result == Result::Renamed && why.isEmpty() && session.renamable() &&
	      session.deviceName() == QStringLiteral("Bench B") &&
	      session.deviceId() == QStringLiteral("EMU-00A1") && identity.name() == "Bench B" &&
	      identity.taken == 1 && identity.steps == 2);
	// The device refuses the name: it stays, and the session says so.
	identity.refuseName = true;
	CHECK(renameTo(QStringLiteral("Refused")));
	CHECK(result == Result::NotRenamed && !why.isEmpty() &&
	      session.deviceName() == QStringLiteral("Bench B") && identity.name() == "Bench B" &&
	      identity.taken == 1 && identity.steps == 2);
	identity.refuseName = false;
	// It takes the name but cannot keep it.
	identity.refuseSave = true;
	CHECK(renameTo(QStringLiteral("Not kept")));
	CHECK(result == Result::RenamedNotKept &&
	      why.contains(QStringLiteral("only until it restarts")) &&
	      session.deviceName() == QStringLiteral("Not kept") && identity.taken == 2 &&
	      identity.steps == 2);
	identity.refuseSave = false;
	// It shortens the name: renamed, to what it serves.
	identity.shortenTo = 5;
	CHECK(renameTo(QStringLiteral("Shortened name")));
	CHECK(result == Result::Renamed && why.isEmpty() &&
	      session.deviceName() == QStringLiteral("Short") && identity.name() == "Short");
	identity.shortenTo = 0;
	// It answers Ok but keeps the old name: not renamed, and the name it serves.
	identity.ignore = true;
	CHECK(renameTo(QStringLiteral("Ignored")));
	CHECK(result == Result::NotRenamed && why.contains(QStringLiteral("Short")) &&
	      session.deviceName() == QStringLiteral("Short"));
	identity.ignore = false;
	// The identity is the protocol's: a host's own WRITE or SAVE to it is
	// refused before it leaves; only rename() writes it.
	const auto identityIndex =
	    session.discoveredPaths().indexOf(QStringLiteral("/telemetry/identity.bin"));
	CHECK(identityIndex >= 0);
	std::array<std::byte, 16 + 4> scratch{};
	const std::array<std::byte, 4> four{};
	CHECK(files::write(session, scratch, static_cast<resource::FileIndex>(identityIndex), 0, four,
	                   true,
	                   [](const files::WriteResult&) noexcept {
	                   })
	          .status == cl::Status::ReadOnly);
	CHECK(files::save(session, static_cast<resource::FileIndex>(identityIndex), 0,
	                  [](const files::SaveResult&) noexcept {
	                  })
	          .status == cl::Status::ReadOnly);
	// A disconnect cuts a rename short, at once and once: before the request
	// leaves, not renamed; in the middle of its save, not known.
	unsigned endings = 0;
	const auto counting = QObject::connect(&session, &qt::ConnectionSession::renameFinished,
	                                       &session, [&](Result, const QString&) {
		                                       ++endings;
	                                       });
	finished = false;
	CHECK(session.rename(QStringLiteral("Never sent")));
	session.disconnect();
	CHECK(finished && result == Result::NotRenamed && !why.isEmpty() && endings == 1 &&
	      !session.renamable() && !session.rename(QStringLiteral("Not connected")));
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	CHECK(await([&] {
		return session.identityState() == qt::ConnectionSession::IdentityState::Known;
	}));
	identity.endless = true;
	const auto stepsBefore = identity.steps;
	finished = false;
	CHECK(session.rename(QStringLiteral("Mid save")));
	CHECK(await([&] {
		return identity.steps > stepsBefore + 3;
	}));
	session.disconnect();
	CHECK(finished && result == Result::Unconfirmed && !why.isEmpty() && endings == 2);
	identity.endless = false;
	QObject::disconnect(counting);
	session.disconnect();
	CHECK(session.identityState() == qt::ConnectionSession::IdentityState::Absent &&
	      session.deviceId().isEmpty());

	// A device that takes a name but has no save(): renamed until it restarts,
	// and the session says so; one with a plain IdentityFile is not renamable.
	{
		TakenIdentity taken;
		CHECK(taken.set("EMU-00B2", "Bench C", "Demo", "0.1.0"));
		tcp::EncodedProfile second(std::span<const std::byte>(packed), descriptor.fingerprint());
		const auto takenFiles = resource::filesystem(
		    second.systemFiles(), resource::file(telemetry::protocol::identity::path, taken));
		Peer temporary(make(second, takenFiles));
		CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"),
		                                  temporary.listener.serverPort()));
		CHECK(await([&] {
			return session.identityState() == qt::ConnectionSession::IdentityState::Known;
		}));
		CHECK(session.renamable() && !session.keepsName());
		CHECK(renameTo(QStringLiteral("Bench D")));
		CHECK(result == Result::RenamedUntilRestart && why.isEmpty() &&
		      session.deviceName() == QStringLiteral("Bench D") && taken.name() == "Bench D");
		session.disconnect();

		resource::telemetry::v3::IdentityFile fixed;
		CHECK(fixed.set("EMU-00C3", "Fixed", "Demo", "0.1.0"));
		tcp::EncodedProfile third(std::span<const std::byte>(packed), descriptor.fingerprint());
		const auto fixedFiles = resource::filesystem(
		    third.systemFiles(), resource::file(telemetry::protocol::identity::path, fixed));
		Peer readOnly(make(third, fixedFiles));
		CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"),
		                                  readOnly.listener.serverPort()));
		CHECK(await([&] {
			return session.identityState() == qt::ConnectionSession::IdentityState::Known;
		}));
		CHECK(!session.renamable() && !session.keepsName() &&
		      !session.rename(QStringLiteral("Never")));
		session.disconnect();
	}
	QObject::disconnect(watching);

	// Renamed on the device: the next connection reads the new name.
	CHECK(identity.rename("Стенд 2"));
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	CHECK(await([&] {
		return session.identityState() == qt::ConnectionSession::IdentityState::Known;
	}));
	CHECK(session.deviceName() == QStringLiteral("Стенд 2") &&
	      session.deviceId() == QStringLiteral("EMU-00A1"));
	session.disconnect();

	// Not served: Absent once ready, and nothing was read for it.
	tcp::EncodedProfile plain(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto without =
	    resource::filesystem(plain.systemFiles(), resource::file("/logs/large.bin", largeFile));
	Peer bare(make(plain, without));
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), bare.listener.serverPort()));
	CHECK(await([&] {
		return session.ready();
	}));
	CHECK(session.identityState() == qt::ConnectionSession::IdentityState::Absent);
	session.disconnect();

	// Something else under its name: Unreadable, the reason, still ready.
	const std::array<std::byte, 16> junk{std::byte{'J'}, std::byte{'U'}, std::byte{'N'},
	                                     std::byte{'K'}};
	const resource::BytesFile junkFile{junk};
	tcp::EncodedProfile third(std::span<const std::byte>(packed), descriptor.fingerprint());
	const auto broken = resource::filesystem(
	    third.systemFiles(), resource::file(telemetry::protocol::identity::path, junkFile));
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

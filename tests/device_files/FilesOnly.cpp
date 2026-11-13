// A device whose files are readable but whose descriptor is missing, too
// small or broken connects files only: telemetry is refused with the reason,
// the listed paths are kept for naming a typo, and the files are still read.
// A device that does not speak the protocol is a plain failure. MIT.
#include <integration/qt/ConnectionSession.hpp>
#include <integration/telemetry/v3/IdentityFile.hpp>
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
#include <vector>
namespace qt = telemetry::integration::qt;
namespace tcp = telemetry::integration::tcp;
namespace files = telemetry::integration::files;
namespace cl = telemetry::client;
using Limitation = qt::ConnectionSession::Limitation;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Files-only check %u failed at line %d\n", checks, line);
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

// Lists 200 bytes, then ends after 100: a broken descriptor file.
struct ShortFile {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 200;
	}

	[[nodiscard]] resource::ReadResult read(resource::Cursor cursor,
	                                        resource::Output output) const noexcept
	{
		constexpr resource::Cursor total = 100;
		if (cursor > total)
			return {resource::Status::InvalidCursor, cursor};
		if (cursor == total)
			return {resource::Status::Ok, cursor, 0, true};
		const auto count = std::min<resource::Cursor>(output.size(), total - cursor);
		std::fill_n(output.begin(), count, std::byte{0x11});
		return {resource::Status::Ok, cursor + count, static_cast<std::uint32_t>(count),
		        cursor + count == total};
	}
};

// Listed and readable by its flags, but every read fails on the device.
struct FailingFile {
	[[nodiscard]] resource::FileSize size() const noexcept
	{
		return 128;
	}

	[[nodiscard]] resource::ReadResult read(resource::Cursor cursor,
	                                        resource::Output) const noexcept
	{
		return {resource::Status::InternalError, cursor};
	}
};

std::array<std::byte, 300> logBytes{};
std::array<std::byte, 10> tinyBytes{};
std::array<std::byte, 128> brokenBytes{};
const resource::BytesFile logFile{logBytes};
const resource::BytesFile tinyFile{tinyBytes};
const resource::BytesFile brokenFile{brokenBytes};
// The owner's case: the descriptor's name is mistyped in the firmware.
const auto typo = resource::filesystem(resource::file("/telemetry/descripter.bin", brokenFile),
                                       resource::file("/logs/events.csv", logFile));
const auto tiny = resource::filesystem(resource::file("/telemetry/descriptor.bin", tinyFile),
                                       resource::file("/logs/events.csv", logFile));
const ShortFile shortFile{};
const FailingFile failingFile{};
const auto shortened = resource::filesystem(resource::file("/telemetry/descriptor.bin", shortFile),
                                            resource::file("/logs/events.csv", logFile));
const auto failing = resource::filesystem(resource::file("/telemetry/descriptor.bin", failingFile),
                                          resource::file("/logs/events.csv", logFile));
const auto broken = resource::filesystem(resource::file("/telemetry/descriptor.bin", brokenFile),
                                         resource::file("/logs/events.csv", logFile));

// A device's identity a client may rename and that keeps it: write() through
// check(), save() at once.
struct RenamableIdentity : resource::telemetry::v3::IdentityFile {
	unsigned saves = 0;

	[[nodiscard]] resource::WriteResult write(resource::Cursor cursor, resource::Input bytes,
	                                          bool final) noexcept
	{
		std::string_view name;
		if (cursor != 0 || !final || check(bytes, name) != resource::Status::Ok || !rename(name))
			return {resource::Status::InvalidData, cursor};
		const auto size = static_cast<std::uint32_t>(bytes.size());
		return {resource::Status::Ok, size, size, true};
	}

	[[nodiscard]] resource::SaveResult save(resource::Cursor cursor) noexcept
	{
		++saves;
		return {resource::Status::Ok, cursor, true};
	}
};

class Peer {
public:
	QTcpServer listener;
	QPointer<QTcpSocket> socket;
	std::unique_ptr<tcp::Server> server;
	alignas(32) std::array<std::byte, model.maxScratch()> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 8192> rx{}, tx{};
	QByteArray pending;
	bool garbage = false;

	explicit Peer(resource::FileSystemView table, bool answerGarbage = false)
	    : garbage(answerGarbage)
	{
		CHECK(listener.listen(QHostAddress::LocalHost, 0));
		QObject::connect(&listener, &QTcpServer::newConnection, &listener, [this, table] {
			auto* candidate = listener.nextPendingConnection();
			socket = candidate;
			pending.clear();
			server = std::make_unique<tcp::Server>(model.view(), workspace, table, rx, tx);
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
		if (garbage) {
			pending.clear();
			socket->write(QByteArray(48, '\x5a'));
			return;
		}
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

bool connected(qt::ConnectionSession& session, Peer& peer)
{
	CHECK(session.connectTcpReference(QStringLiteral("127.0.0.1"), peer.listener.serverPort()));
	return await([&] {
		return session.ready() || session.filesOnly() ||
		       (!session.connecting() && !session.lastError().isEmpty());
	});
}

// The file at index in the device's list, read whole through the session.
std::vector<std::byte> readWhole(qt::ConnectionSession& session, resource::FileIndex index)
{
	std::vector<std::byte> bytes;
	Reader reader{session, Sink{&bytes}};
	CHECK(reader.start(index) == cl::Status::Ok);
	CHECK(await([&] {
		(void)reader.poll();
		return reader.finished();
	}));
	CHECK(reader.state() == files::ReadState::Complete);
	return bytes;
}

void filesStillWork(qt::ConnectionSession& session)
{
	CHECK(!session.ready() && session.filesOnly() && session.filesSupported());
	// lastError() is for a failed connection; this one is open, with a reason.
	CHECK(session.lastError().isEmpty() && !session.limitationText().isEmpty() &&
	      !session.recoveryRequired());
	// This Session branch captures callbacks too; it must refuse a null one
	// without leaving work for a later event-loop turn to invoke.
	std::array<std::byte, resource::protocol::wire::listRequestSize> request{};
	CHECK(resource::protocol::client::makeList(request, 0).status ==
	      resource::protocol::client::BuildStatus::Ok);
	void (*nullCompletion)(const cl::ResultView&) noexcept = nullptr;
	const auto refusedFile = session.submitResource(request, files::maxReplyBytes, nullCompletion);
	CHECK(!refusedFile && refusedFile.status == cl::Status::InvalidArgument && !session.busy());
	// Telemetry is refused; the files are not.
	const auto refused = session.readFieldValue(
	    0, [](const telemetry::integration::dynamic::DynamicResult&) noexcept {
	    });
	CHECK(!refused);
	const auto log = readWhole(session, 1);
	CHECK(log.size() == logBytes.size() &&
	      std::equal(logBytes.begin(), logBytes.end(), log.begin()));
}

void noDescriptor()
{
	Peer peer{typo.view()};
	qt::ConnectionSession session;
	CHECK(connected(session, peer));
	CHECK(session.limitation() == Limitation::NoDescriptor);
	CHECK((session.discoveredPaths() == QStringList{QStringLiteral("/telemetry/descripter.bin"),
	                                                QStringLiteral("/logs/events.csv")}));
	filesStillWork(session);
	// The mistyped file itself can be read for inspection.
	CHECK(readWhole(session, 0).size() == brokenBytes.size());
	session.disconnect();
	CHECK(!session.filesOnly() && !session.filesSupported() &&
	      session.limitation() == Limitation::None);
}

void descriptorTooSmall()
{
	Peer peer{tiny.view()};
	qt::ConnectionSession session;
	CHECK(connected(session, peer));
	CHECK(session.limitation() == Limitation::DescriptorSize &&
	      session.refusedDescriptorSize() == 10);
	filesStillWork(session);
	CHECK(readWhole(session, 0).size() == tinyBytes.size());
}

void descriptorBroken()
{
	for (std::size_t i = 0; i < brokenBytes.size(); ++i)
		brokenBytes[i] = std::byte(0xc3 ^ i);
	Peer peer{broken.view()};
	qt::ConnectionSession session;
	CHECK(connected(session, peer));
	// Read whole, then rejected by the parser: still files only.
	CHECK(session.limitation() == Limitation::DescriptorRejected);
	CHECK(session.limitationText().startsWith(QStringLiteral("Telemetry is not available: ")));
	filesStillWork(session);
	const auto descriptorBytes = readWhole(session, 0);
	CHECK(descriptorBytes.size() == brokenBytes.size() &&
	      std::equal(brokenBytes.begin(), brokenBytes.end(), descriptorBytes.begin()));
}

void descriptorShort()
{
	Peer peer{shortened.view()};
	qt::ConnectionSession session;
	CHECK(connected(session, peer));
	// Framing intact, the file broken: files only, and the file is the evidence.
	CHECK(session.limitation() == Limitation::DescriptorRejected);
	filesStillWork(session);
}

void descriptorUnreadable()
{
	Peer peer{failing.view()};
	qt::ConnectionSession session;
	CHECK(connected(session, peer));
	CHECK(session.limitation() == Limitation::DescriptorUnreadable);
	filesStillWork(session);
}

void notTheProtocol()
{
	Peer peer{typo.view(), true};
	qt::ConnectionSession session;
	CHECK(connected(session, peer));
	CHECK(!session.ready() && !session.filesOnly() && !session.filesSupported());
	CHECK(session.limitation() == Limitation::None && !session.lastError().isEmpty());
}
} // namespace

// A files-only device is renamed as any other: its identity's WRITE - with a
// 24-character MCU id, longer than a files-only queue once held - goes on the
// files-only queue, then SAVE, then the read-back.
void renamedFilesOnly()
{
	RenamableIdentity identity;
	CHECK(identity.set("0123456789ABCDEF01234567", "Bench analyzer", "PA-3", "1.4.2"));
	const auto table =
	    resource::filesystem(resource::file("/telemetry/descripter.bin", brokenFile),
	                         resource::file(telemetry::protocol::identity::path, identity));
	Peer peer{table.view()};
	qt::ConnectionSession session;
	CHECK(connected(session, peer));
	CHECK(session.filesOnly() && await([&] {
		      return session.identityState() == qt::ConnectionSession::IdentityState::Known;
	      }));
	CHECK(session.renamable() && session.keepsName());
	bool finished = false;
	auto result = qt::ConnectionSession::RenameResult::NotRenamed;
	QObject::connect(&session, &qt::ConnectionSession::renameFinished, &session,
	                 [&](qt::ConnectionSession::RenameResult ended, const QString&) {
		                 finished = true;
		                 result = ended;
	                 });
	CHECK(session.rename(QStringLiteral("Bench B")));
	CHECK(await([&] {
		return finished;
	}));
	CHECK(result == qt::ConnectionSession::RenameResult::Renamed &&
	      session.deviceName() == QStringLiteral("Bench B") && identity.name() == "Bench B" &&
	      identity.saves == 1);
	session.disconnect();
}

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	for (std::size_t i = 0; i < logBytes.size(); ++i)
		logBytes[i] = std::byte('a' + i % 26);
	noDescriptor();
	descriptorTooSmall();
	descriptorBroken();
	descriptorShort();
	descriptorUnreadable();
	notTheProtocol();
	renamedFilesOnly();
	std::printf("Files-only connection: %u checks, 0 failures\n", checks);
	return 0;
}

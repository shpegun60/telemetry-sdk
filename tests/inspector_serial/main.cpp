/*
 * @file main.cpp
 * @brief Exercise real QSerialPort on host PTYs with existing COBS and native Model dispatch.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "DemoPeer.hpp"
#include "InspectorController.hpp"
#include <integration/qt/SerialTransport.hpp>
#include "../../lib/integration/serial/Profile.hpp"
#include "../../examples/nucleo_h7s3l8/Protocol.hpp"
#include <cobs/Cobs.h>
#include <resource/BytesFile.hpp>
#include <resource/FileSystem.hpp>
#include <resource/protocol/Packets.hpp>
#include <resource/protocol/Protocol.hpp>
#include <wire/EncodedWire.hpp>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace ui = inspector_qml;
namespace ew = telemetry::protocol::encoded;

namespace {
unsigned checks = 0, reads = 0, admissions = 0;

#define CHECK(expression)                                                                          \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(expression)) {                                                                       \
			std::fprintf(stderr, "Inspector serial check %u failed at line %d: %s\n", checks,      \
			             __LINE__, #expression);                                                   \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

void timingPolicyChecks()
{
	using telemetry::integration::serial::exchangeTimeoutMs;
	static_assert(exchangeTimeoutMs(32, 2048, 1200, 3000) == 20500);
	static_assert(exchangeTimeoutMs(13, 885, 1200, 5000) == 12609);
	CHECK(telemetry::integration::serial::FrameFormat::tx_storage_size_for_capacity(32) == 38);
	CHECK(telemetry::integration::serial::FrameFormat::tx_storage_size_for_capacity(2048) == 2062);
	CHECK(exchangeTimeoutMs(32, 544, 1200, 3000) == 7917);
	CHECK(exchangeTimeoutMs(32, 2048, 115200, 3000) == 3183);
	CHECK(exchangeTimeoutMs(32, 2048, 4000000, 3000) == 3006);
	CHECK(exchangeTimeoutMs(2049, 32, 1200, 3000) == 0);
	CHECK(exchangeTimeoutMs(32, 2049, 1200, 3000) == 0);
	CHECK(exchangeTimeoutMs(32, 32, 1199, 3000) == 0);
	CHECK(exchangeTimeoutMs(32, 32, 4000001, 3000) == 0);
	CHECK(exchangeTimeoutMs(32, 32, 1200, UINT32_MAX) == 0);
}

#ifdef Q_OS_UNIX
template<class Predicate>
bool await(Predicate predicate, int milliseconds = 6000)
{
	QElapsedTimer elapsed;
	elapsed.start();
	do {
		QCoreApplication::processEvents();
		if (predicate())
			return true;
		QTest::qWait(1);
	} while (elapsed.elapsed() < milliseconds);
	return predicate();
}

resource::Input input(const QByteArray& bytes)
{
	return {reinterpret_cast<const std::byte*>(bytes.constData()),
	        static_cast<std::size_t>(bytes.size())};
}

struct WrongCrc : crc::Codec<std::uint16_t> {
	std::uint16_t calculate(std::span<const std::uint8_t> bytes) const noexcept
	{
		return std::uint16_t(crc::Crc16Bitwise{}.calculate(bytes) ^ 1);
	}
};

template<class Integrity = crc::Crc16Bitwise>
QByteArray frame(const QByteArray& body)
{
	using Encoder =
	    cobs::Endpoint<wire::Pool<1, 1>,
	                   cobs::Format<Integrity, telemetry::integration::serial::maxBodyBytes>>;
	Encoder encoder;
	QByteArray result;
	CHECK(encoder.bind(
	    [&](std::span<const std::uint8_t> bytes) noexcept {
		    result = {reinterpret_cast<const char*>(bytes.data()),
		              static_cast<qsizetype>(bytes.size())};
		    return true;
	    },
	    []() noexcept {
		    return false;
	    }));
	auto message = encoder.make_message(static_cast<std::size_t>(body.size()));
	CHECK(message && message.append_bytes(std::span<const std::uint8_t>{
	                     reinterpret_cast<const std::uint8_t*>(body.constData()),
	                     static_cast<std::size_t>(body.size())}));
	CHECK(encoder.send(message) == wire::SendResult::Sent);
	encoder.poll(0);
	CHECK(!encoder.tx_active());
	return result;
}

QByteArray fieldReply(quint32 session, quint32 id, quint32 endpoint, bool unavailable = false)
{
	std::array<std::byte, 40> bytes{};
	ew::Header header;
	header.reply = true;
	header.session = session;
	header.requestId = id;
	header.endpoint = endpoint;
	header.expectedResponseBytes = 8;
	header.payloadBytes = unavailable ? 0 : 8;
	header.dispatch = unavailable ? 9 : 0;
	CHECK(ew::writeHeader(bytes, header) == ew::Status::Ok);
	return {reinterpret_cast<const char*>(bytes.data()), unavailable ? 32 : 40};
}

namespace paced_fixture {
inline constexpr auto data = [] {
	std::array<std::uint8_t, 512> result{};
	result.fill(90);
	return result;
}();

const auto& read() noexcept
{
	return data;
}

inline constexpr telemetry::Model model{
    telemetry::group("timing", telemetry::field<&read>("Payload"))};
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto packed = telemetry::descriptor::packDescriptor<descriptor>();
} // namespace paced_fixture

class Peer {
public:
	enum class Profile {
		Demo,
		Board,
		Paced
	};

private:
	struct File {
		QByteArray bytes;
		std::size_t chunkBytes = 113;

		resource::FileSize size() const noexcept
		{
			return static_cast<resource::FileSize>(bytes.size());
		}

		resource::ReadResult read(resource::Cursor cursor, resource::Output out) const noexcept
		{
			return resource::BytesFile{input(bytes)}.read(
			    cursor, out.first(std::min(out.size(), chunkBytes)));
		}
	};

	using Link = cobs::Endpoint<wire::Pool<4, 1>, telemetry::integration::serial::FrameFormat>;
	int fd_ = -1;
	QString path_;
	ui::DemoPeer native_;
	nucleo_demo::Protocol board_;
	Profile profile_;
	telemetry::Workspace pacedWorkspace_{std::span<std::byte>{}};
	File decoy_{QByteArrayLiteral("version 1")}, descriptor_;
	resource::FileSystem<2> files_;
	Link link_;
	QTimer timer_;
	std::deque<QByteArray> tx_;
	qsizetype offset_ = 0;
	std::array<std::byte, 2048> response_{};
	QByteArray previous_;
	bool pumping_ = false;
	QElapsedTimer elapsed_;
	qint64 txStarted_ = 0;
	bool txClockStarted_ = false;

public:
	qsizetype fragmentBytes = 0;
	unsigned responseBaud = 0;
	bool corruptNext = false, dropNext = false, unavailableNext = false, prependPrevious = false;
	bool resourcePartialTail = false;
	unsigned wrongVoltage = 0, altered = 0;
	unsigned listBarriers = 0, stats = 0, descriptorReads = 0, fieldReads = 0;
	quint64 receivedBytes = 0;

	explicit Peer(Profile profile = Profile::Demo)
	    : profile_(profile), descriptor_{[&] {
		      const auto bytes = profile == Profile::Board
		                             ? std::span<const std::byte>{nucleo_demo::packedDescriptor}
		                         : profile == Profile::Paced
		                             ? std::span<const std::byte>{paced_fixture::packed}
		                             : native_.descriptor();
		      return QByteArray{reinterpret_cast<const char*>(bytes.data()),
		                        static_cast<qsizetype>(bytes.size())};
	      }()},
	      files_(resource::filesystem(resource::file("/device/version.bin", decoy_),
	                                  resource::file("/telemetry/descriptor.bin", descriptor_)))
	{
		elapsed_.start();
		int slave = -1;
		char name[256]{};
		termios raw{};
		CHECK(openpty(&fd_, &slave, name, nullptr, nullptr) == 0);
		CHECK(tcgetattr(slave, &raw) == 0);
		cfmakeraw(&raw);
		CHECK(tcsetattr(slave, TCSANOW, &raw) == 0);
		CHECK(fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK) == 0);
		path_ = QString::fromLocal8Bit(name);
		::close(slave);
		CHECK(link_.bind(
		    [&](std::span<const std::uint8_t> bytes) noexcept {
			    tx_.push_back({reinterpret_cast<const char*>(bytes.data()),
			                   static_cast<qsizetype>(bytes.size())});
			    return true;
		    },
		    [&]() noexcept {
			    return !tx_.empty();
		    }));
		timer_.setInterval(1);
		QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] {
			proceed();
		});
		timer_.start();
	}

	~Peer()
	{
		timer_.stop();
		tx_.clear();
		link_.poll(0);
		(void)link_.unbind();
		::close(fd_);
	}

	QString path() const
	{
		return path_;
	}

	void inject(QByteArray bytes)
	{
		tx_.push_back(std::move(bytes));
	}

	QByteArray descriptor() const
	{
		return descriptor_.bytes;
	}

	void setDescriptorReadBytes(std::size_t count)
	{
		descriptor_.chunkBytes = count;
	}

	void proceed()
	{
		if (pumping_)
			return;
		pumping_ = true;
		std::array<std::uint8_t, 512> bytes{};
		for (unsigned i = 0; i < 16; ++i) {
			const auto got = ::read(fd_, bytes.data(), bytes.size());
			if (got <= 0)
				break;
			receivedBytes += static_cast<quint64>(got);
			link_.consume(
			    std::span<const std::uint8_t>{bytes}.first(static_cast<std::size_t>(got)));
		}
		link_.poll(0);
		if (tx_.empty() && !link_.tx_active() && link_.has_packet()) {
			QByteArray body;
			{
				auto packet = link_.pop_packet();
				const auto data = packet.data();
				body = {reinterpret_cast<const char*>(data.data()),
				        static_cast<qsizetype>(data.size())};
			}
			std::size_t written = 0;
			const bool encoded = body.startsWith("TEQ1");
			if (encoded) {
				const auto parsed = ew::parse(input(body));
				CHECK(parsed && !parsed.header.reply);
				++fieldReads;
				written = profile_ == Profile::Board ? board_.dispatch(input(body), response_, 0)
				          : profile_ == Profile::Paced
				              ? telemetry::integration::encoded::dispatchEncoded(
				                    paced_fixture::model.view(), pacedWorkspace_,
				                    parsed.header.session, input(body), response_)
				                    .written
				              : native_.dispatch(input(body), response_, parsed.header.session);
				if (profile_ == Profile::Board && wrongVoltage != 0 && written == 44) {
					++altered;
					const float changed = wrongVoltage == 1   ? 230.11f
					                      : wrongVoltage == 2 ? 229.81f
					                                          : 231.21f;
					const auto bits = std::bit_cast<std::uint32_t>(changed);
					for (unsigned byte = 0; byte < 4; ++byte)
						response_[32 + (wrongVoltage - 1) * 4 + byte] =
						    std::byte(bits >> (byte * 8));
				}
			} else {
				CHECK(!body.isEmpty());
				const auto op = static_cast<unsigned char>(body[0]);
				if (op == 1)
					++listBarriers;
				if (op == 2)
					++stats;
				if (op == 3)
					++descriptorReads;
				written =
				    resource::protocol::process(files_.view(), input(body), response_).written;
			}
			CHECK(written > 0 && written <= response_.size());
			auto reply = QByteArray(reinterpret_cast<const char*>(response_.data()),
			                        static_cast<qsizetype>(written));
			if (encoded && unavailableNext) {
				const auto request = ew::parse(input(body));
				reply = fieldReply(request.header.session, request.header.requestId,
				                   request.header.endpoint, true);
				unavailableNext = false;
			}
			if (encoded && prependPrevious && !previous_.isEmpty()) {
				inject(frame(previous_));
				prependPrevious = false;
			}
			if (encoded)
				previous_ = reply;
			if (encoded && dropNext)
				dropNext = false;
			else {
				if (encoded && corruptNext) {
					inject(frame<WrongCrc>(reply));
					corruptNext = false;
				}
				if (!encoded && resourcePartialTail) {
					const auto framed = frame(reply);
					inject(framed + framed.left(4));
					resourcePartialTail = false;
				} else {
					inject(frame(reply));
				}
			}
		}
		if (!tx_.empty()) {
			auto& block = tx_.front();
			const auto remaining = block.size() - offset_;
			auto count = fragmentBytes > 0 ? std::min(remaining, fragmentBytes) : remaining;
			if (responseBaud != 0) {
				if (!txClockStarted_) {
					txStarted_ = elapsed_.elapsed();
					txClockStarted_ = true;
				}
				const auto available = (elapsed_.elapsed() - txStarted_) * responseBaud / 10000;
				count = std::min(count, std::max(qsizetype(0), qsizetype(available) - offset_));
			}
			const auto sent =
			    ::write(fd_, block.constData() + offset_, static_cast<std::size_t>(count));
			if (sent > 0) {
				offset_ += sent;
				if (offset_ == block.size()) {
					tx_.pop_front();
					offset_ = 0;
					txClockStarted_ = false;
				}
			}
		}
		link_.poll(0);
		pumping_ = false;
	}
};

QModelIndex endpoint(ui::EndpointModel& model, quint32 id)
{
	const auto fields = model.index(0, 0);
	for (int c = 0; c < model.rowCount(fields); ++c) {
		const auto group = model.index(c, 0, fields);
		for (int r = 0; r < model.rowCount(group); ++r) {
			const auto node = model.index(r, 0, group);
			if (model.endpoint(node).value("id").toUInt() == id)
				return node;
		}
	}
	return {};
}

void controllerReads()
{
	Peer peer;
	peer.fragmentBytes = 7;
	ui::InspectorController client;
	auto& ends = *qobject_cast<ui::EndpointModel*>(client.endpoints());
	auto& values = *qobject_cast<ui::ValueModel*>(client.values());
	QSignalSpy completed{&client, &ui::InspectorController::readFinished};
	CHECK(client.serialAvailable() && client.connectSerial(peer.path(), 115200));
	if (!await([&] {
		    return client.connected();
	    })) {
		std::fprintf(stderr, "%s\n", qPrintable(client.statusText()));
		CHECK(false);
	}
	++admissions;
	CHECK(peer.listBarriers == 1 && peer.stats == 1 && peer.descriptorReads > 1);
	client.setTcpWriteChunkBytes(1); // A TCP fault knob must not alter the active serial adapter.
	const auto read = [&](quint32 id) {
		CHECK(client.selectEndpoint(endpoint(ends, id)));
		const auto before = completed.count();
		CHECK(client.refresh());
		CHECK(await([&] {
			return !client.busy();
		}));
		CHECK(completed.count() == before + 1 && completed.last()[0].toBool() &&
		      values.rowCount() == 1);
		++reads;
	};
	read(65536);
	CHECK(values.data(values.index(0, 0), ui::ValueModel::ValueRole).toString() ==
	      "18446744073709551615");
	peer.corruptNext = true;
	read(65537);
	CHECK(values.data(values.index(0, 0), ui::ValueModel::ValueRole).toString() ==
	      "-9223372036854775808");
	peer.prependPrevious = true;
	read(65538);
	CHECK(values.data(values.index(0, 0), ui::ValueModel::ValueRole).toString() == "-2 (unknown)");
	read(131074);
	CHECK(values.canFetchMore(values.index(0, 0)));
	values.fetchMore(values.index(0, 0));
	CHECK(values.rowCount(values.index(0, 0)) == 3);
	read(131073);
	CHECK(!values.hasChildren(values.index(0, 0)));
	const auto beforeLarge = peer.fieldReads;
	CHECK(client.selectEndpoint(endpoint(ends, 131072)));
	CHECK(!client.refresh());
	CHECK(!client.busy() && client.inspector().pending() == 0 && peer.fieldReads == beforeLarge);
	CHECK(client.selectEndpoint(endpoint(ends, 65536)));
	peer.unavailableNext = true;
	CHECK(client.refresh());
	CHECK(await([&] {
		return !client.busy();
	}));
	CHECK(!completed.last()[0].toBool() && client.connected());
	read(65536);
	client.disconnectDevice();
	CHECK(!client.connected() && !client.cleanupPending());
	CHECK(client.connectSerial(peer.path(), 115200));
	CHECK(await([&] {
		return client.connected();
	}));
	++admissions;
	peer.prependPrevious = true;
	read(65537);
	CHECK(peer.listBarriers == 2);
	peer.dropNext = true;
	client.setReadTimeout(60);
	CHECK(client.selectEndpoint(endpoint(ends, 65536)));
	CHECK(client.refresh());
	CHECK(await([&] {
		return !client.busy();
	}));
	CHECK(!completed.last()[0].toBool());
	client.disconnectDevice();
	CHECK(client.cleanupPending());
	client.setReadTimeout(1000);
	CHECK(client.connectSerial(peer.path(), 115200));
	CHECK(await([&] {
		return client.connected();
	}));
	++admissions;
	CHECK(!client.cleanupPending());
	read(65536);
}

void transportBoundsAndShortWrite()
{
	Peer peer;
	telemetry::integration::qt::SerialTransport port;
	QSignalSpy descriptors{&port, &telemetry::integration::qt::DeviceTransport::descriptorReceived};
	QSignalSpy errors{&port, &telemetry::integration::qt::DeviceTransport::transportError};
	QSignalSpy packets{&port, &telemetry::integration::qt::DeviceTransport::packetReceived};
	port.connectTo(peer.path());
	CHECK(await([&] {
		return descriptors.count() == 1;
	}));
	CHECK(port.ready());
	const auto identity = port.peerAddress();
	const auto observed = QJsonDocument::fromJson(identity.toUtf8()).object();
	CHECK(observed.value("systemLocation").toString() ==
	      QFileInfo(peer.path()).canonicalFilePath());
	CHECK(observed.contains("serialNumber") && observed.contains("vendorId") &&
	      observed.contains("productId"));
	std::array<std::byte, 32> request{};
	ew::Header header;
	header.session = 11;
	header.requestId = 1;
	header.endpoint = 65536;
	header.expectedResponseBytes = 32768;
	CHECK(ew::writeHeader(request, header) == ew::Status::Ok);
	CHECK(!port.stage({request, 11, 1}));
	CHECK(peer.fieldReads == 0);
	header.expectedResponseBytes = 8;
	CHECK(ew::writeHeader(request, header) == ew::Status::Ok);
	CHECK(await([&] {
		return port.canStage();
	}));
	CHECK(port.stage({request, 11, 1}));
	port.discardStaged();
	CHECK(port.canStage());
	CHECK(port.stage({request, 11, 1}));
	port.setWriteChunkBytes(1);
	const auto bytesBefore = peer.receivedBytes;
	CHECK(!port.sendStaged());
	CHECK(await([&] {
		return errors.count() == 1;
	}));
	QTest::qWait(40);
	CHECK(!port.ready() && port.locallyQuiescent() && packets.isEmpty() && peer.fieldReads == 0);
	CHECK(peer.receivedBytes - bytesBefore <=
	      1); // No repeated prefix, no automatic remainder or request replay.
	port.setWriteChunkBytes(0);
	port.connectTo(peer.path());
	CHECK(await([&] {
		return descriptors.count() == 2;
	}));
	CHECK(port.ready());
	header.session = 12;
	CHECK(ew::writeHeader(request, header) == ew::Status::Ok);
	CHECK(port.stage({request, 12, 1}));
	CHECK(port.sendStaged());
	CHECK(await([&] {
		return packets.count() == 1;
	}));
	CHECK(packets.last()[1].toUInt() == 12 && packets.last()[2].toUInt() == 1);
	port.close();
	CHECK(port.locallyQuiescent());
	CHECK(port.peerAddress() ==
	      identity); // Identity is the observation made at open, not a later lookup.
	Peer other;
	port.connectTo(other.path());
	CHECK(await([&] {
		return descriptors.count() == 3;
	}));
	CHECK(port.peerAddress() != identity);
	port.close();
}

void staleResourceTranscript()
{
	for (unsigned kind = 0; kind < 3; ++kind) {
		Peer peer;
		std::array<std::byte, 256> request{};
		resource::protocol::client::BuildResult built{};
		if (kind == 0)
			built = resource::protocol::client::makeList(request, 0);
		else if (kind == 1)
			built = resource::protocol::client::makeStat(request, 1);
		else
			built = resource::protocol::client::makeRead(request, 1, 0);
		CHECK(built.status == resource::protocol::client::BuildStatus::Ok);
		// A captured old normal response is delivered before the new server sees LIST.
		QByteArray old;
		if (kind == 0) {
			const auto path = QByteArrayLiteral("/telemetry/descriptor.bin");
			old.resize(14 + path.size());
			old.fill('\0');
			old[1] = 1;
			old[9] = 1;
			const auto count = path.size() + 2;
			old[10] = char(count);
			old[11] = char(count >> 8);
			old[12] = char(path.size());
			std::copy(path.begin(), path.end(), old.begin() + 14);
		} else if (kind == 1) {
			old = QByteArray(6, '\0');
			old[1] = 64;
			old[5] = 1;
		} else {
			old = QByteArray(12 + 64, '\0');
			old[1] = 64;
			old[9] = 1;
			old[10] = 64;
		}
		peer.inject(frame(old));
		telemetry::integration::qt::SerialTransport port;
		QSignalSpy admitted{&port,
		                    &telemetry::integration::qt::DeviceTransport::descriptorReceived};
		QSignalSpy errors{&port, &telemetry::integration::qt::DeviceTransport::transportError};
		port.connectTo(peer.path());
		CHECK(await([&] {
			return !admitted.isEmpty() || !errors.isEmpty();
		}));
		CHECK(admitted.isEmpty());
		CHECK(!port.ready());
		port.close();
		CHECK(port.locallyQuiescent());
	}
}

void resourceTailAndQueuedReconnect()
{
	Peer peer;
	peer.resourcePartialTail = true;
	telemetry::integration::qt::SerialTransport port;
	QSignalSpy admitted{&port, &telemetry::integration::qt::DeviceTransport::descriptorReceived};
	QSignalSpy errors{&port, &telemetry::integration::qt::DeviceTransport::transportError};
	port.connectTo(peer.path());
	CHECK(await([&] {
		return !errors.isEmpty();
	}));
	CHECK(admitted.isEmpty() && !port.ready() && peer.listBarriers == 1 && peer.stats == 0);
	CHECK(port.locallyQuiescent());
	Peer abandoned, current;
	port.connectTo(abandoned.path());
	port.close();
	port.connectTo(current.path());
	CHECK(await([&] {
		return admitted.count() == 1;
	}));
	CHECK(port.ready() && errors.count() == 1);
	CHECK(abandoned.listBarriers == 0 && current.listBarriers == 1);
	port.close();
}

void lowBaudDeadlines()
{
	Peer bootstrap{Peer::Profile::Board}, field{Peer::Profile::Paced},
	    shortField{Peer::Profile::Paced}, shortBootstrap{Peer::Profile::Board};
	bootstrap.setDescriptorReadBytes(
	    2036); // One real Descriptor reply exceeds the old five seconds.
	bootstrap.responseBaud = 1200;
	shortBootstrap.responseBaud = 1200;
	ui::InspectorController bootstrapClient, fieldClient, shortClient;
	telemetry::integration::qt::SerialTransport shortPort;
	QSignalSpy completed{&fieldClient, &ui::InspectorController::readFinished};
	QSignalSpy shortCompleted{&shortClient, &ui::InspectorController::readFinished};
	QSignalSpy shortErrors{&shortPort,
	                       &telemetry::integration::qt::DeviceTransport::transportError};
	QSignalSpy shortDescriptors{&shortPort,
	                            &telemetry::integration::qt::DeviceTransport::descriptorReceived};
	CHECK(fieldClient.connectSerial(field.path(), 1200));
	CHECK(shortClient.connectSerial(shortField.path(), 1200));
	CHECK(await([&] {
		return fieldClient.connected() && shortClient.connected();
	}));
	admissions += 2;
	auto& fields = *qobject_cast<ui::EndpointModel*>(fieldClient.endpoints());
	auto& shortFields = *qobject_cast<ui::EndpointModel*>(shortClient.endpoints());
	CHECK(fieldClient.selectEndpoint(endpoint(fields, 0)));
	CHECK(shortClient.selectEndpoint(endpoint(shortFields, 0)));
	field.responseBaud = shortField.responseBaud = 1200;
	shortClient.setReadTimeout(
	    80); // Exact caller deadline must not be raised by the automatic policy.
	QElapsedTimer elapsed;
	elapsed.start();
	CHECK(fieldClient.refresh());
	CHECK(shortClient.refresh());
	CHECK(shortCompleted.isEmpty() && shortField.fieldReads == 0);
	// Deliberately withhold the owner event loop past the explicit deadline.
	// qSleep does not dispatch Qt events, so no pump can authorize this request.
	QTest::qSleep(100);
	shortClient.pump();
	std::fprintf(stderr,
	             "Inspector Serial pre-handoff timeout: busy=%d connected=%d recovery=%d "
	             "fieldReads=%u\n",
	             shortClient.busy(), shortClient.connected(),
	             shortClient.inspector().recoveryRequired(), shortField.fieldReads);
	CHECK(shortCompleted.count() == 1 && !shortCompleted.last()[0].toBool());
	CHECK(!shortClient.busy());
	CHECK(shortClient.connected());
	CHECK(!shortClient.inspector().recoveryRequired());
	CHECK(shortField.fieldReads == 0);
	CHECK(bootstrapClient.connectSerial(bootstrap.path(), 1200));
	shortPort.setTimeout(80);
	shortPort.connectTo(shortBootstrap.path(), 1200);
	CHECK(await(
	    [&] {
		    return !shortCompleted.isEmpty() && !shortErrors.isEmpty();
	    },
	    1500));
	const auto explicitElapsed = elapsed.elapsed();
	CHECK(explicitElapsed < 1500);
	CHECK(shortCompleted.count() == 1 && !shortCompleted.last()[0].toBool());
	CHECK(!shortClient.busy() && shortClient.connected());
	CHECK(!shortClient.inspector().recoveryRequired());
	CHECK(shortDescriptors.isEmpty() && !shortPort.ready() && shortPort.locallyQuiescent());
	CHECK(await(
	    [&] {
		    return !completed.isEmpty();
	    },
	    8000));
	const auto fieldElapsed = elapsed.elapsed();
	CHECK(fieldElapsed > 3000); // Actual framed byte pacing crosses the old core deadline.
	CHECK(completed.count() == 1 && completed.last()[0].toBool() && fieldClient.connected());
	CHECK(field.fieldReads == 1 && shortField.fieldReads == 0);
	++reads;
	auto& values = *qobject_cast<ui::ValueModel*>(fieldClient.values());
	CHECK(values.rowCount() == 1);
	const auto root = values.index(0, 0);
	CHECK(values.data(root, ui::ValueModel::ChildCountRole).toUInt() == 512);
	values.fetchMore(root);
	CHECK(values.rowCount(root) == 128);
	CHECK(values.data(values.index(0, 0, root), ui::ValueModel::ValueRole).toString() == "90");
	CHECK(await(
	    [&] {
		    return bootstrapClient.connected();
	    },
	    10000));
	const auto bootstrapElapsed = elapsed.elapsed();
	CHECK(bootstrapElapsed > 5000 && bootstrap.descriptorReads == 1);
	CHECK(bootstrap.listBarriers == 1 && bootstrap.stats == 1);
	++admissions;
	// No duplicate completion or execution after the Resource peer's late frame.
	CHECK(shortCompleted.count() == 1 && shortErrors.count() == 1 && shortDescriptors.isEmpty());
	CHECK(shortField.fieldReads == 0 && shortClient.connected() &&
	      !shortClient.inspector().recoveryRequired());
	std::printf("Inspector Serial low-baud: bootstrap=1 field=1 explicit=2 bootstrapMs=%lld "
	            "fieldMs=%lld explicitMs=%lld\n",
	            static_cast<long long>(bootstrapElapsed), static_cast<long long>(fieldElapsed),
	            static_cast<long long>(explicitElapsed));
}

void deviceCheckProcesses(const QString& executable, const QString& receipts)
{
	CHECK(QFileInfo::exists(executable));
	CHECK(QDir{}.mkpath(receipts));
	for (unsigned wrong = 0; wrong < 4; ++wrong) {
		Peer peer{Peer::Profile::Board};
		peer.fragmentBytes = 11;
		peer.wrongVoltage = wrong;
		const auto name = QStringLiteral("device-check-%1").arg(wrong);
		const auto report = QDir(receipts).filePath(name + QStringLiteral(".json"));
		QProcess child;
		child.setProcessChannelMode(QProcess::MergedChannels);
		child.start(executable, {QStringLiteral("--serial"), peer.path(), QStringLiteral("--baud"),
		                         QStringLiteral("115200"), QStringLiteral("--check-device"),
		                         QStringLiteral("--report"), report});
		CHECK(child.waitForStarted(5000));
		CHECK(await(
		    [&] {
			    return child.state() == QProcess::NotRunning;
		    },
		    30000));
		const auto output = child.readAll();
		QFile log{QDir(receipts).filePath(name + QStringLiteral(".log"))};
		CHECK(log.open(QIODevice::WriteOnly) && log.write(output) == output.size());
		if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != (wrong ? 9 : 0))
			std::fprintf(stderr, "Checker %u: %s\n", wrong, output.constData());
		CHECK(child.exitStatus() == QProcess::NormalExit && child.exitCode() == (wrong ? 9 : 0));
		QFile result{report};
		CHECK(result.open(QIODevice::ReadOnly));
		const auto observed = QJsonDocument::fromJson(result.readAll()).object();
		CHECK(observed.value("passed").toBool() == (wrong == 0));
		CHECK(observed.value("voltage").toBool() == (wrong == 0));
		CHECK(observed.value("exactCounter").toBool() && observed.value("demonstration").toBool());
		CHECK(observed.value("probe").toBool());
		CHECK(observed.value("fields").toArray().size() == 4);
		CHECK(observed.value("scalarCount").toInt() >= 16);
		CHECK(peer.fieldReads == 4 && peer.listBarriers == 1 && peer.stats == 1);
		CHECK(peer.altered == (wrong ? 1u : 0u));
	}
}
#endif
} // namespace

int main(int argc, char** argv)
{
	QGuiApplication app{argc, argv};
	timingPolicyChecks();
	telemetry::integration::qt::SerialTransport idle;
	CHECK(!idle.ready() && idle.locallyQuiescent());
	idle.close();
	CHECK(idle.locallyQuiescent());
#ifdef Q_OS_UNIX
	controllerReads();
	transportBoundsAndShortWrite();
	staleResourceTranscript();
	resourceTailAndQueuedReconnect();
	lowBaudDeadlines();
	QTemporaryDir temporary;
	CHECK(temporary.isValid());
	deviceCheckProcesses(argc > 1 ? QString::fromLocal8Bit(argv[1])
	                              : QCoreApplication::applicationDirPath() +
	                                    QStringLiteral("/example/telemetry-inspector-qml"),
	                     argc > 2 ? QString::fromLocal8Bit(argv[2]) : temporary.path());
	const int pty = 1;
#else
	const int pty = 0;
#endif
	std::printf("Inspector Serial checks=%u reads=%u admissions=%u pty=%d Qt=%s\n", checks, reads,
	            admissions, pty, qVersion());
	return 0;
}

/*
 * @file Consumer.cpp
 * @brief Ordinary QObject consumer of installed optional transport modules.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <integration/qt/DescriptorBootstrap.hpp>
#include <integration/qt/DeviceTransport.hpp>
#ifdef TEST_SERIAL
#include <integration/qt/SerialTransport.hpp>
#endif
#ifdef TEST_CONNECTION
#include <integration/qt/ConnectionSession.hpp>
#endif
#include <QCoreApplication>
#include <QString>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace qt = telemetry::integration::qt;
static unsigned checks = 0;
#define CHECK(condition)                                                                           \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(condition))                                                                          \
			std::abort();                                                                          \
	} while (false)

class Receiver final : public QObject {
	Q_OBJECT
public:
	int received = 0;
signals:
	void observed();
public slots:

	void receive(QByteArray bytes)
	{
		CHECK(bytes == QByteArray("descriptor"));
		++received;
		emit observed();
	}
};

class Transport final : public qt::DeviceTransport {
public:
	using DeviceTransport::DeviceTransport;

	void close() override
	{}

	bool ready() const noexcept override
	{
		return false;
	}

	bool canStage() const noexcept override
	{
		return false;
	}

	bool stage(telemetry::client::Packet) override
	{
		return false;
	}

	bool sendStaged() override
	{
		return false;
	}

	void discardStaged() override
	{}

	bool locallyQuiescent() const noexcept override
	{
		return true;
	}

	QString peerAddress() const override
	{
		return {};
	}

	void setWriteChunkBytes(int) noexcept override
	{}

	void setTimeout(int) noexcept override
	{}
};

int main(int argc, char** argv)
{
	QCoreApplication application(argc, argv);
	Transport transport;
	Receiver receiver;
	int observed = 0;
	QObject::connect(&transport, &qt::DeviceTransport::descriptorReceived, &receiver,
	                 &Receiver::receive);
	QObject::connect(&receiver, &Receiver::observed, &application, [&] {
		++observed;
	});
	emit transport.descriptorReceived(QByteArray("descriptor"));
	CHECK(receiver.received == 1);
	CHECK(observed == 1);
	CHECK(!transport.ready());
	CHECK(transport.locallyQuiescent());
	qt::DescriptorBootstrap bootstrap;
	CHECK(!bootstrap.complete());
	bootstrap.reset();
	std::array<std::byte, 256> bytes{};
	const auto built = bootstrap.request(bytes);
	CHECK(built.status == resource::protocol::client::BuildStatus::Ok && built.written > 0);
#ifdef TEST_SERIAL
	qt::SerialTransport serial;
	CHECK(!serial.ready());
	CHECK(!serial.canStage());
	CHECK(serial.locallyQuiescent());
	CHECK(serial.peerAddress().isEmpty());
	CHECK(qt::SerialTransport::maxBodyBytes() == 2048);
	CHECK(qt::SerialTransport::minBaudRate() == 1200);
	CHECK(qt::SerialTransport::maxBaudRate() == 4000000);
	const auto maximum = qt::SerialTransport::maxBodyBytes();
	const auto slow = qt::SerialTransport::exchangeTimeoutMs(maximum, maximum, 1200, 3000);
	const auto fast = qt::SerialTransport::exchangeTimeoutMs(maximum, maximum, 4000000, 3000);
	CHECK(slow > fast && slow <= 60000 && fast >= 3000);
	CHECK(qt::SerialTransport::exchangeTimeoutMs(maximum + 1, maximum, 1200, 3000) == 0);
	CHECK(qt::SerialTransport::exchangeTimeoutMs(maximum, maximum + 1, 1200, 3000) == 0);
	CHECK(qt::SerialTransport::exchangeTimeoutMs(1, 1, 1199, 3000) == 0);
	serial.close();
	serial.close();
	CHECK(!serial.ready() && !serial.canStage() && serial.locallyQuiescent());
#endif
#ifdef TEST_CONNECTION
	qt::ConnectionSession connection;
	CHECK(!connection.ready() && !connection.connecting() && !connection.recoveryRequired());
	CHECK(!connection.workbench() && !connection.storage());
	CHECK(!connection.connectTcpReference(QString{}, 0));
	CHECK(!connection.lastError().isEmpty());
	CHECK(connection.reserveSessionsThrough(42));
	CHECK(!connection.reserveSessionsThrough(UINT32_MAX));
	connection.process();
	connection.disconnect();
	CHECK(!connection.ready() && !connection.recoveryRequired());
	connection.close();
	CHECK(!connection.connectTcpReference(QStringLiteral("127.0.0.1"), 5577));
	CHECK(!connection.workbench() && !connection.storage());
#endif

	std::printf("Installed Qt transport checks=%u\n", checks);
}

#include "Consumer.moc"

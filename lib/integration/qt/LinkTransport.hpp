/*
 * @file LinkTransport.hpp
 * @brief Resource discovery and Encoded exchange over any Link and Framer.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 *
 * Everything a DeviceTransport does that does not depend on the medium lives
 * here: sequential Descriptor discovery, one outstanding exchange, bounded
 * writes, the exchange and discovery deadlines, and the order in which replies
 * and errors are delivered. A medium contributes a Link (its byte channel) and
 * a Framer (its frame boundaries); a subclass adds how its link is addressed
 * and calls one of the discover functions to connect.
 */
#ifndef TELEMETRY_INTEGRATION_QT_LINK_TRANSPORT_HPP
#define TELEMETRY_INTEGRATION_QT_LINK_TRANSPORT_HPP
#pragma once

#include <integration/link/Framer.hpp>
#include <integration/qt/DeviceTransport.hpp>
#include <integration/qt/Link.hpp>
#include <memory>

namespace telemetry::integration::qt {
class LinkTransport : public telemetry::integration::qt::DeviceTransport {

	Q_OBJECT

public:
	struct Options {
		// Bounds the whole discovery, independently of each exchange's deadline.
		int discoveryTimeoutMs = 60000;
		// The medium's name in error text, as in "TCP exchange timed out".
		QString medium;
	};

	LinkTransport(std::unique_ptr<Link> link,
	              std::unique_ptr<telemetry::integration::link::Framer> framer, Options options,
	              QObject* parent = nullptr);
	~LinkTransport() override;
	QByteArray readManyCapability() const;
	QByteArray batchCapability() const;
	void close() override;
	bool ready() const noexcept override;
	bool canStage() const noexcept override;
	bool stage(telemetry::client::Packet packet) override;
	bool sendStaged() override;
	void discardStaged() override;
	bool locallyQuiescent() const noexcept override;
	QString peerAddress() const override;
	void setWriteChunkBytes(int bytes) noexcept override;
	void setTimeout(int milliseconds) noexcept override;

protected:
	Link& link() noexcept;
	// Close whatever is open, open the link and discover the Descriptor.
	void discover();
	// The same, also admitting the optional ReadMany or Encoded (batch) profile
	// under requestedSession. A zero session is refused with transportError().
	void discoverWithReadMany(quint32 requestedSession);
	void discoverWithEncoded(quint32 requestedSession);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace telemetry::integration::qt
#endif

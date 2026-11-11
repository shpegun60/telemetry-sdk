/*
 * @file Link.hpp
 * @brief One medium's byte channel under a LinkTransport.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 *
 * A link opens, carries bytes and reports that it is gone; it knows nothing of
 * frames, requests or deadlines. The transport reads and writes through
 * device(), which stays the same object for the link's whole life, and reacts
 * to its readyRead() and bytesWritten() signals.
 *
 * Contract:
 * - open() starts opening with the settings the subclass was given and
 *   returns at once; opened() follows when bytes can flow.
 * - lost() reports, with a reason a person can read, that the channel failed
 *   or the peer went away. It may come more than once for one loss and may
 *   come during close(); the transport ignores it once it has closed.
 * - close() drops the channel at once, discarding anything not yet written.
 * - quiescent() is true when nothing is open, opening or still to be written.
 */
#ifndef TELEMETRY_INTEGRATION_QT_LINK_HPP
#define TELEMETRY_INTEGRATION_QT_LINK_HPP
#pragma once

#include <QtCore/QIODevice>
#include <QtCore/QObject>
#include <QtCore/QString>

namespace telemetry::integration::qt {
class Link : public QObject {
	Q_OBJECT
public:
	explicit Link(QObject* parent = nullptr) : QObject(parent)
	{}

	~Link() override = default;
	virtual QIODevice& device() noexcept = 0;
	virtual void open() = 0;
	virtual void close() = 0;
	virtual bool quiescent() const noexcept = 0;
	// The far end as a person would name it; empty when there is none.
	virtual QString peer() const = 0;
Q_SIGNALS:
	void opened();
	void lost(QString reason);
};
} // namespace telemetry::integration::qt
#endif

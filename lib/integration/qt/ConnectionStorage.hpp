/*
 * @file ConnectionStorage.hpp
 * @brief Optional Qt-owned native session storage and one transport progress path.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_QT_CONNECTION_STORAGE_HPP
#define TELEMETRY_INTEGRATION_QT_CONNECTION_STORAGE_HPP
#pragma once

#include <integration/qt/DeviceTransport.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/workbench/WorkbenchModel.hpp>
#include <wire/ReadManyWire.hpp>
#include <QtCore/QElapsedTimer>
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace telemetry::integration::qt {
using HostWorkbench = workbench::WorkbenchModel<8, 4096, 65536, 768>;

// Advanced host storage seam. It is shared only to retain native callback
// backing; there is one Workbench, DynamicClient and FIFO. Qt allocations never
// enter the portable module. The Descriptor and workspace outlive that client.
class ConnectionStorage {
public:
	struct Reply {
		std::vector<std::byte> bytes;
		quint32 session = 0, id = 0, due = 0;
	};

	explicit ConnectionStorage(QByteArray bytes);
	virtual ~ConnectionStorage();
	ConnectionStorage(const ConnectionStorage&) = delete;
	ConnectionStorage& operator=(const ConnectionStorage&) = delete;
	quint32 now() const noexcept;
	client::Input input() const noexcept;
	QString initialize(quint32 session, bool admit = true);
	QString admitSession(quint32 session);
	// All calls are owner-thread only. Stage copies packet bytes before the
	// owner-only handoff guard; only then can sendStaged authorize physical TX.
	client::Packet progressTransport(DeviceTransport* transport);
	void settleReplies();
	bool acceptPacket(quint32 session, quint32 id) noexcept;
	bool receivePacket(const QByteArray& bytes, quint32 session, quint32 id);
	// The caller closes/releases physical transport before invoking this. It
	// settles complete already-owned replies, closes once, then drains callbacks.
	void closeAndDrain();

	QElapsedTimer elapsed;
	QByteArray descriptor;
	std::vector<runtime::RuntimeType> types;
	std::vector<runtime::RuntimeMember> members;
	std::vector<runtime::RuntimeEnumEntry> enums;
	std::vector<runtime::RuntimeCatalog> catalogs;
	std::vector<runtime::RuntimeEndpoint> endpoints;
	std::unique_ptr<runtime::RuntimeSchemaWorkspace> storage;
	runtime::RuntimeSchema schema;
	std::optional<protocol::encoded::read_many::Capability> readMany;
	std::array<std::byte, 65568> tx{}, rx{};
	client::Clock clock{this, +[](void* owner) noexcept {
		                    return static_cast<ConnectionStorage*>(owner)->now();
	                    }};
	encoded::EncodedBackend backend{tx, rx, clock, 30000, true};
	std::array<std::byte, 65536> serviceStorage{};
	HostWorkbench inspector{backend, clock, serviceStorage};
	std::vector<Reply> replies;
	bool mutationAuthorized = false, remoteAccepted = false;
	quint32 remoteRequestId = 0;
};
} // namespace telemetry::integration::qt
#endif

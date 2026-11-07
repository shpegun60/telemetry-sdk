/*
 * @file ConnectionStorage.cpp
 * @brief Descriptor ownership, guarded TX and deadline-ordered RX for Qt sessions.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include <integration/qt/ConnectionStorage.hpp>
#include <wire/EncodedWire.hpp>
#include <cstdlib>

namespace telemetry::integration::qt {
ConnectionStorage::ConnectionStorage(QByteArray bytes) : descriptor(std::move(bytes))
{
	elapsed.start();
}

ConnectionStorage::~ConnectionStorage()
{
	closeAndDrain();
}

quint32 ConnectionStorage::now() const noexcept
{
	return static_cast<quint32>(elapsed.elapsed());
}

client::Input ConnectionStorage::input() const noexcept
{
	return {reinterpret_cast<const std::byte*>(descriptor.constData()),
	        static_cast<std::size_t>(descriptor.size())};
}

QString ConnectionStorage::initialize(quint32 session, bool admit)
{
	if (storage)
		return QStringLiteral("Connection storage cannot be initialized twice");
	runtime::RuntimeSchemaWorkspace sizing{{}, {}, {}, {}, {}};
	const auto required = runtime::RuntimeSchema::parse(input(), sizing);
	if (!required && required.status != runtime::Status::Capacity)
		return QStringLiteral("Descriptor rejected (status %1, error %2)")
		    .arg(static_cast<unsigned>(required.status))
		    .arg(static_cast<unsigned>(required.error));
	types.resize(required.requirements.types);
	members.resize(required.requirements.members);
	enums.resize(required.requirements.enumEntries);
	catalogs.resize(required.requirements.catalogs);
	endpoints.resize(required.requirements.endpoints);
	storage = std::make_unique<runtime::RuntimeSchemaWorkspace>(types, members, enums, catalogs,
	                                                            endpoints);
	const auto parsed = runtime::RuntimeSchema::parse(input(), *storage);
	if (!parsed)
		return QStringLiteral("Descriptor rejected (status %1, error %2)")
		    .arg(static_cast<unsigned>(parsed.status))
		    .arg(static_cast<unsigned>(parsed.error));
	schema = parsed.schema;
	return admit ? admitSession(session) : QString{};
}

QString ConnectionStorage::admitSession(quint32 session)
{
	const auto status = inspector.admit(schema, input().first(64), session);
	return status == client::Status::Ok
	           ? QString{}
	           : QStringLiteral("Admission refused (status %1)").arg(static_cast<unsigned>(status));
}

client::Packet ConnectionStorage::progressTransport(DeviceTransport* transport)
{
	inspector.process(now());
	if (remoteAccepted) {
		remoteAccepted = false;
		(void)inspector.onSendAccepted(inspector.session(), remoteRequestId);
	}
	const auto packet = inspector.nextPacket();
	if (transport && packet && transport->canStage() && transport->stage(packet)) {
		if (inspector.prepareHandoff(packet.session, packet.requestId)) {
			const auto parsed = protocol::encoded::parse(packet.bytes);
			if (packet.mayExecute && parsed &&
			    parsed.header.operation != protocol::encoded::Operation::FieldRead &&
			    parsed.header.operation != protocol::encoded::Operation::ReadMany)
				mutationAuthorized = true;
			remoteRequestId = packet.requestId;
			if (!transport->sendStaged())
				inspector.invalidate();
		} else
			transport->discardStaged();
	}
	return packet;
}

void ConnectionStorage::settleReplies()
{
	const auto time = now();
	for (auto it = replies.begin(); it != replies.end();) {
		if (!client::expired(time, it->due)) {
			++it;
			continue;
		}
		(void)inspector.onReply(it->bytes, it->session, it->id);
		it = replies.erase(it);
	}
	inspector.process(time);
}

bool ConnectionStorage::acceptPacket(quint32 session, quint32 id) noexcept
{
	if (session != inspector.session() || id != remoteRequestId)
		return false;
	remoteAccepted = true;
	return true;
}

bool ConnectionStorage::receivePacket(const QByteArray& bytes, quint32 session, quint32 id)
{
	if (session != inspector.session() || id != remoteRequestId || !replies.empty() ||
	    bytes.size() < 0 || static_cast<std::size_t>(bytes.size()) > rx.size())
		return false;
	const auto* first = reinterpret_cast<const std::byte*>(bytes.constData());
	replies.push_back({{first, first + bytes.size()}, session, id, now()});
	return true;
}

void ConnectionStorage::closeAndDrain()
{
	// Never progress TX here. onReply enforces the existing absolute deadline;
	// queued readback from a successful Write is closed before another handoff.
	for (const auto& reply : replies)
		if (client::expired(now(), reply.due))
			(void)inspector.onReply(reply.bytes, reply.session, reply.id);
	replies.clear();
	inspector.close();
	for (unsigned i = 0; i < 16 && !inspector.drained(); ++i)
		inspector.process(now());
	if (!inspector.drained())
		std::abort();
}
} // namespace telemetry::integration::qt

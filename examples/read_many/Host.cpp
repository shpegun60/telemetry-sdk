/* One dynamic FIFO, three cross-catalog Fields, real Encoded dispatcher. MIT. */
#include "Model.hpp"
#include <integration/read_many/Client.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <resource/BytesFile.hpp>
#include <algorithm>
#include <array>
#include <cstdio>

namespace cl = telemetry::client;
namespace rt = telemetry::runtime;
namespace dyn = telemetry::integration::dynamic;
namespace many = telemetry::integration::read_many;
namespace encoded = telemetry::integration::encoded;
namespace wire = telemetry::protocol::encoded::read_many;

int main()
{
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 1> members{};
	std::array<rt::RuntimeEnumEntry, 1> enums{};
	std::array<rt::RuntimeCatalog, 6> catalogs{}; // two groups, three families
	std::array<rt::RuntimeEndpoint, 8> endpoints{};
	rt::RuntimeSchemaWorkspace metadata{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(peer::descriptorBytes, metadata);
	if (!parsed)
		return 1;
	const auto schema = parsed.schema;
	const auto* voltage = schema.findField("measure", "Voltage_mV");
	const auto* energy = schema.findField("measure", "Energy");
	const auto* enabled = schema.findField("control", "Enabled");
	if (!voltage || !energy || !enabled)
		return 2;

	// Resource owns no Telemetry knowledge. An application chooses this file's
	// ID/path and publishes an immutable record for the admitted Model/session.
	const wire::Capability advertised{1, schema.fingerprint()};
	std::array<std::byte, wire::capabilityBytes> capStorage{}, received{};
	if (!wire::writeCapability(capStorage, advertised))
		return 3;
	resource::BytesFile capabilityFile{capStorage};
	const auto read = capabilityFile.read(0, received);
	if (read.status != resource::Status::Ok || read.written != received.size())
		return 4;
	const auto observed = wire::parseCapability(received);
	if (!observed)
		return 5;

	std::uint32_t now = 0;
	cl::Clock clock{&now, +[](void* p) noexcept {
		                return *static_cast<std::uint32_t*>(p);
	                }};
	std::array<std::byte, 546> tx{}, rx{}, portTx{}, reply{};
	encoded::EncodedBackend backend{tx, rx, clock, 30000, true};
	// Explicit callback storage: the accepted batch copies its IDs/types.
	dyn::DynamicClient<4, 130, 640> client{backend, clock};
	if (client.admit(schema, schema.descriptor().first(64), 1) != cl::Status::Ok)
		return 6;
	const std::array ids{voltage->id, energy->id, enabled->id};
	const auto binding = many::bindReadMany(client, backend, observed.value, ids);
	if (!binding)
		return 7;
	bool complete = false;
	const auto submission =
	    many::readMany(client, binding.binding, [&](const many::ReadManyResult& result) noexcept {
		    if (!result.operation.succeeded() || result.valueStatus != dyn::ValueStatus::Ok ||
		        result.count != 3 || !result.items[0].hasValue() || !result.items[1].hasValue() ||
		        !result.items[2].hasValue())
			    return;
		    const auto v = result.items[0].value.get<std::uint32_t>();
		    const auto e = result.items[1].value.get<std::uint64_t>();
		    const auto b = result.items[2].value.get<bool>();
		    complete = v && e && b && v.value == 230100 && e.value == UINT64_MAX && b.value;
		    if (complete)
			    std::printf("Voltage_mV=%u Energy=%llu Enabled=true\n", unsigned(v.value),
			                static_cast<unsigned long long>(e.value));
	    });
	if (!submission)
		return 8;
	std::array<std::byte, 64> scratch{};
	telemetry::Workspace workspace{scratch};
	for (unsigned step = 0; step < 8 && client.pending(); ++step) {
		client.process(now);
		const auto packet = client.nextPacket();
		if (!packet)
			continue;
		// This synchronous port retains its own copy until the exchange ends.
		// A DMA port must retain that copy until physical TX release as well.
		std::copy(packet.bytes.begin(), packet.bytes.end(), portTx.begin());
		if (!client.prepareHandoff(packet.session, packet.requestId))
			return 9;
		const auto dispatched = encoded::dispatchEncoded(
		    peer::model.view(), workspace, packet.session,
		    cl::Input{portTx}.first(packet.bytes.size()), reply, advertised);
		if (!dispatched || !client.onSendAccepted(packet.session, packet.requestId) ||
		    !client.onReply(cl::Input{reply}.first(dispatched.written), packet.session,
		                    packet.requestId))
			return 10;
	}
	client.close();
	while (!client.drained())
		client.process(now);
	return complete ? 0 : 11;
}

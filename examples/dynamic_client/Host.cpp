/* Runtime read/edit/write/refresh through the real host Model. MIT. */
#include <integration/dynamic/DynamicClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/runtime/DynamicValueWriter.hpp>

#include <algorithm>
#include <array>
#include <cstdio>

namespace cl = telemetry::client;
namespace rt = telemetry::runtime;
namespace dyn = telemetry::integration::dynamic;
namespace encoded = telemetry::integration::encoded;

// Only the simulated peer needs the native type. The client below reads and
// edits its wire representation using the received Descriptor and prepared paths.
namespace peer {
struct Config {
	std::uint32_t baudRate;
	float gain;
	bool enabled;
};

struct Device {
	Config config{9600, 1.0f, true};

	Config read() const noexcept
	{
		return config;
	}

	telemetry::WriteResult write(const Config& value) noexcept
	{
		config = value;
		return telemetry::WriteResult::Applied;
	}
};

Device device;
constexpr telemetry::Model model{
    telemetry::group("device", telemetry::field<&Device::read, &Device::write>("Config", device))};
constexpr telemetry::descriptor::Descriptor descriptor{model};
constexpr auto descriptorBytes = telemetry::descriptor::packDescriptor<descriptor>();
} // namespace peer

int main()
{
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 16> members{};
	std::array<rt::RuntimeEnumEntry, 1> enums{};
	std::array<rt::RuntimeCatalog, 4> catalogs{};
	std::array<rt::RuntimeEndpoint, 8> endpoints{};
	rt::RuntimeSchemaWorkspace storage{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(peer::descriptorBytes, storage);
	if (!parsed)
		return 1;
	const auto schema = parsed.schema;
	const auto* field = schema.findField("device", "Config");
	if (!field)
		return 2;
	const auto baud = schema.resolve(field->typeId, "baudRate");
	if (!baud)
		return 3;

	std::uint32_t now = 0;
	cl::Clock clock{&now, +[](void* context) noexcept {
		                return *static_cast<std::uint32_t*>(context);
	                }};
	std::array<std::byte, 128> tx{}, response{}, portTx{}, reply{};
	encoded::EncodedBackend backend{tx, response, clock};
	dyn::DynamicClient<4, 128> client{backend, clock};
	if (client.admit(schema, schema.descriptor().first(64), 1) != cl::Status::Ok)
		return 4;
	const auto read = client.bind(cl::Operation::FieldRead, field->id);
	const auto write = client.bind(cl::Operation::FieldWrite, field->id);
	if (!read || !write)
		return 5;

	alignas(peer::Config) std::array<std::byte, sizeof(peer::Config) + alignof(peer::Config)>
	    scratch{};
	telemetry::Workspace workspace{scratch};
	// This synchronous host port owns a TX copy for the entire exchange. A real
	// asynchronous port retains its copy until the driver's physical TX release.
	auto pump = [&]() noexcept {
		for (unsigned step = 0; step < 64 && client.pending(); ++step) {
			client.process(now);
			const auto packet = client.nextPacket();
			if (!packet)
				continue;
			std::copy(packet.bytes.begin(), packet.bytes.end(), portTx.begin());
			if (!client.prepareHandoff(packet.session, packet.requestId))
				return false;
			const auto request = cl::Input{portTx}.first(packet.bytes.size());
			const auto result = encoded::dispatchEncoded(peer::model.view(), workspace,
			                                             packet.session, request, reply);
			if (!result || !client.onSendAccepted(packet.session, packet.requestId) ||
			    !client.onReply(cl::Input{reply}.first(result.written), packet.session,
			                    packet.requestId))
				return false;
		}
		return client.pending() == 0;
	};

	std::array<std::byte, 9> original{}, staging{}, edited{};
	bool ok = false;
	auto first = client.submit(read.endpoint, {}, [&](const dyn::DynamicResult& result) noexcept {
		ok = result.hasValue() && result.operation.payload.size() == original.size();
		if (ok)
			std::copy(result.operation.payload.begin(), result.operation.payload.end(),
			          original.begin());
	});
	if (!first || !pump() || !ok)
		return 6;
	// The first callback's borrowed payload is no longer used. Edit its owned copy.
	auto editor = rt::DynamicValueWriter::begin(schema, field->typeId, original, staging);
	if (editor.set(baud.value, std::uint32_t{115200}) != rt::Status::Ok || !editor.commit(edited))
		return 7;
	ok = false;
	auto changed =
	    client.submit(write.endpoint, edited, [&](const dyn::DynamicResult& result) noexcept {
		    ok = result.operation.succeeded() && result.valueStatus == dyn::ValueStatus::NoValue;
	    });
	bool refreshed = false;
	auto refresh = client.submit(read.endpoint, {}, [&](const dyn::DynamicResult& result) noexcept {
		if (!result.hasValue())
			return;
		const auto actual = result.value.get<std::uint32_t>(baud.value);
		refreshed = actual && actual.value == 115200;
		if (actual)
			std::printf("Applied baudRate: %u\n", unsigned(actual.value));
	});
	if (!changed || !refresh || !pump() || !ok || !refreshed)
		return 8;
	client.close();
	while (!client.drained())
		client.process(now);
	return 0;
}

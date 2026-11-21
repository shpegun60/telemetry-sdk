/* One ordered Write/Command/Service/Read through one DynamicClient FIFO. MIT. */
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <integration/batch/Client.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/BatchDispatcher.hpp>
#include <algorithm>
#include <array>
#include <cstdio>

namespace ts = telemetry;
namespace cl = ts::client;
namespace rt = ts::runtime;
namespace dy = ts::integration::dynamic;
namespace bt = ts::integration::batch;
namespace en = ts::integration::encoded;

struct Argument {
	std::uint32_t value;
};

struct Device {
	std::uint32_t value = 0;

	std::uint32_t read() const noexcept
	{
		return value;
	}

	ts::WriteResult write(const std::uint32_t& input) noexcept
	{
		value = std::min(input, std::uint32_t{100});
		return ts::WriteResult::Applied;
	}

	ts::CommandResult add(const Argument& amount) noexcept
	{
		value += amount.value;
		return ts::CommandResult::Executed;
	}

	ts::ServiceResult<Argument> twice(const Argument& input) noexcept
	{
		return ts::ServiceResult<Argument>::success({input.value * 2});
	}
};

static Device device;
static constexpr ts::Model model{ts::group(
    "device", ts::field<&Device::read, &Device::write>("Value", device),
    ts::command<&Device::add>("Add", device), ts::service<&Device::twice>("Twice", device))};
static constexpr ts::descriptor::Descriptor descriptor{model};
static constexpr auto descriptorBytes = ts::descriptor::packDescriptor<descriptor>();

int main()
{
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 1> members{};
	std::array<rt::RuntimeEnumEntry, 1> enums{};
	std::array<rt::RuntimeCatalog, 3> catalogs{};
	std::array<rt::RuntimeEndpoint, 3> endpoints{};
	rt::RuntimeSchemaWorkspace metadata{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(descriptorBytes, metadata);
	if (!parsed)
		return 1;
	const auto schema = parsed.schema;
	const auto* field = schema.findField("device", "Value");
	// This example has one catalog per family. Enumerate the public runtime
	// endpoints instead of assuming compile-time Command or Service IDs.
	const auto named = [&](ts::descriptor::Category category,
	                       std::string_view name) -> const rt::RuntimeEndpoint* {
		for (const auto& endpoint : schema.endpoints(category))
			if (endpoint.name == name)
				return &endpoint;
		return nullptr;
	};
	const auto* command = named(ts::descriptor::Category::Command, "Add");
	const auto* service = named(ts::descriptor::Category::Service, "Twice");
	if (!field || !command || !service)
		return 2;
	std::uint32_t now = 0;
	cl::Clock clock{&now, +[](void* p) noexcept {
		                return *static_cast<std::uint32_t*>(p);
	                }};
	std::array<std::byte, 256> tx{}, rx{}, port{}, reply{}, request{}, scratch{};
	bool complete = false;
	en::EncodedBackend backend{tx, rx, clock, 30000, false, true};
	dy::DynamicClient<2, 256, 1536> client{backend, clock};
	if (client.admit(schema, schema.descriptor().first(64), 1) != cl::Status::Ok)
		return 3;
	// In a remote application this independent TBM1 record comes from discovery.
	const bt::wire::Capability capability{1, schema.fingerprint(), 32, 256, 256};
	const std::array selected{bt::Endpoint{cl::Operation::FieldWrite, field->id},
	                          bt::Endpoint{cl::Operation::Command, command->id},
	                          bt::Endpoint{cl::Operation::Service, service->id},
	                          bt::Endpoint{cl::Operation::FieldRead, field->id}};
	const auto bound = bt::bindBatch(client, backend, capability, selected);
	if (!bound)
		return 4;
	std::array<std::byte, 4> write{}, add{}, argument{};
	if (ts::encode(std::uint32_t{99}, write) != ts::CodecStatus::Ok ||
	    ts::encode(Argument{1}, add) != ts::CodecStatus::Ok ||
	    ts::encode(Argument{14}, argument) != ts::CodecStatus::Ok)
		return 5;
	const std::array<cl::Input, 4> payloads{write, add, argument, {}};
	const auto submitted = bt::batch(
	    client, bound.binding, payloads, request, [&](const bt::BatchResult& result) noexcept {
		    if (!result.operation.succeeded() || result.valueStatus != bt::ValueStatus::Ok ||
		        result.count != 4)
			    return;
		    for (std::size_t i = 0; i < result.count; ++i)
			    if (!result.items[i].operation.succeeded())
				    return;
		    if (!result.items[2].hasValue() || !result.items[3].hasValue())
			    return;
		    const auto member = result.items[2].value.member("value");
		    if (!member)
			    return;
		    const auto doubled = member.value.get<std::uint32_t>();
		    const auto read = result.items[3].value.get<std::uint32_t>();
		    complete = doubled && read && doubled.value == 28 && read.value == 100;
		    if (complete)
			    std::printf("Write Applied; Command Executed; Service=%u; Read=%u\n",
			                unsigned(doubled.value), unsigned(read.value));
	    });
	if (!submitted)
		return 6;
	ts::Workspace workspace{scratch};
	for (unsigned step = 0; step < 8 && client.pending(); ++step) {
		client.process(now);
		const auto packet = client.nextPacket();
		if (!packet)
			continue;
		std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
		if (!client.prepareHandoff(packet.session, packet.requestId))
			return 7;
		const auto dispatched =
		    en::dispatchEncoded(model.view(), workspace, packet.session,
		                        cl::Input{port}.first(packet.bytes.size()), reply, capability);
		if (!dispatched || !client.onSendAccepted(packet.session, packet.requestId) ||
		    !client.onReply(cl::Input{reply}.first(dispatched.written), packet.session,
		                    packet.requestId))
			return 8;
	}
	client.close();
	while (!client.drained())
		client.process(now);
	return complete ? 0 : 9;
}

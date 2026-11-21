/* Real mixed batches across catalog families and nonconsecutive packed IDs. MIT. */
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <integration/batch/Client.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/BatchDispatcher.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace ts = telemetry;
namespace cl = ts::client;
namespace rt = ts::runtime;
namespace dy = ts::integration::dynamic;
namespace bt = ts::integration::batch;
namespace en = ts::integration::encoded;
namespace ew = ts::protocol::encoded;
static unsigned checks = 0;

static void check(bool ok, unsigned line) noexcept
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "batch cross-group check=%u line=%u\n", checks, line);
		std::abort();
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

struct Argument {
	std::uint32_t value;
};

struct Event {
	cl::Operation operation;
	unsigned group, index;
	std::uint32_t value;
	friend bool operator==(const Event&, const Event&) = default;
};

static std::array<Event, 32> events{};
static std::size_t eventCount = 0;

struct Device {
	unsigned group;
	std::array<std::uint32_t, 4> values{};

	void note(cl::Operation operation, unsigned index, std::uint32_t value) noexcept
	{
		CHECK(eventCount < events.size());
		events[eventCount++] = {operation, group, index, value};
	}

	template<unsigned Index>
	std::uint32_t read() noexcept
	{
		note(cl::Operation::FieldRead, Index, values[Index]);
		return values[Index];
	}

	template<unsigned Index>
	ts::WriteResult write(const std::uint32_t& input) noexcept
	{
		note(cl::Operation::FieldWrite, Index, input);
		values[Index] = input;
		return ts::WriteResult::Applied;
	}

	template<unsigned Index>
	ts::CommandResult command(const Argument& input) noexcept
	{
		note(cl::Operation::Command, Index, input.value);
		values[Index] += input.value;
		return ts::CommandResult::Executed;
	}

	template<unsigned Index>
	ts::ServiceResult<Argument> service(const Argument& input) noexcept
	{
		note(cl::Operation::Service, Index, input.value);
		return ts::ServiceResult<Argument>::success(
		    {1000 * group + 100 * Index + values[Index] + input.value});
	}
};

static std::array<Device, 4> devices{{{0}, {1}, {2}, {3}}};

// Four groups of one owner each, every group with F0..F3, C0..C3 and S0..S3.
static constexpr auto branch(const char* name, Device& device) noexcept
{
	return ts::group(name, ts::field<&Device::read<0>, &Device::write<0>>("F0", device),
	                 ts::field<&Device::read<1>, &Device::write<1>>("F1", device),
	                 ts::field<&Device::read<2>, &Device::write<2>>("F2", device),
	                 ts::field<&Device::read<3>, &Device::write<3>>("F3", device),
	                 ts::command<&Device::command<0>>("C0", device),
	                 ts::command<&Device::command<1>>("C1", device),
	                 ts::command<&Device::command<2>>("C2", device),
	                 ts::command<&Device::command<3>>("C3", device),
	                 ts::service<&Device::service<0>>("S0", device),
	                 ts::service<&Device::service<1>>("S1", device),
	                 ts::service<&Device::service<2>>("S2", device),
	                 ts::service<&Device::service<3>>("S3", device));
}

static constexpr ts::Model model{branch("decoy", devices[0]), branch("one", devices[1]),
                                 branch("two", devices[2]), branch("three", devices[3])};
static constexpr ts::descriptor::Descriptor descriptor{model};
static constexpr auto descriptorBytes = ts::descriptor::packDescriptor<descriptor>();

int main()
{
	for (auto& device : devices)
		for (unsigned i = 0; i < device.values.size(); ++i)
			device.values[i] = 100 * device.group + i;
	const auto original = devices;
	std::array<rt::RuntimeType, 64> types{};
	std::array<rt::RuntimeMember, 4> members{};
	std::array<rt::RuntimeEnumEntry, 1> enums{};
	std::array<rt::RuntimeCatalog, 12> catalogs{};
	std::array<rt::RuntimeEndpoint, 48> endpoints{};
	rt::RuntimeSchemaWorkspace metadata{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(descriptorBytes, metadata);
	CHECK(parsed);
	const auto schema = parsed.schema;
	// Validate actual descriptor positions, independently of the selected list.
	for (const auto category : {ts::descriptor::Category::Field, ts::descriptor::Category::Command,
	                            ts::descriptor::Category::Service}) {
		const auto groups = schema.catalogs(category);
		const auto entries = schema.endpoints(category);
		CHECK(groups.size() == 4 && entries.size() == 16);
		for (unsigned group = 0; group < 4; ++group) {
			CHECK(groups[group].endpointCount == 4);
			for (unsigned index = 0; index < 4; ++index) {
				const auto& entry = entries[groups[group].firstEndpoint + index];
				CHECK(ts::groupOf(entry.id) == group && ts::indexOf(entry.id) == index);
			}
		}
	}
	const auto* field = schema.findField("one", "F3");
	CHECK(field && field->id == ts::idOf<model, "one/F3">());

	const std::array<bt::Endpoint, 12> selected{{
	    {cl::Operation::FieldRead, ts::idOf<model, "three/F2">()},
	    {cl::Operation::FieldWrite, ts::idOf<model, "one/F3">()},
	    {cl::Operation::Command, ts::idOf<model, "two/C1">()},
	    {cl::Operation::Service, ts::idOf<model, "one/S3">()},
	    {cl::Operation::FieldRead, ts::idOf<model, "one/F3">()},
	    {cl::Operation::FieldRead, ts::idOf<model, "two/F1">()},
	    {cl::Operation::Command, ts::idOf<model, "one/C3">()},
	    {cl::Operation::Service, ts::idOf<model, "three/S0">()},
	    {cl::Operation::Service, ts::idOf<model, "two/S1">()},
	    {cl::Operation::FieldRead, ts::idOf<model, "one/F3">()},
	    {cl::Operation::FieldWrite, ts::idOf<model, "three/F0">()},
	    {cl::Operation::FieldRead, ts::idOf<model, "three/F0">()},
	}};
	// Literal oracle does not derive routing or order from the request builder.
	const std::array<Event, 12> expected{{
	    {cl::Operation::FieldRead, 3, 2, 302},
	    {cl::Operation::FieldWrite, 1, 3, 700},
	    {cl::Operation::Command, 2, 1, 5},
	    {cl::Operation::Service, 1, 3, 7},
	    {cl::Operation::FieldRead, 1, 3, 700},
	    {cl::Operation::FieldRead, 2, 1, 206},
	    {cl::Operation::Command, 1, 3, 4},
	    {cl::Operation::Service, 3, 0, 9},
	    {cl::Operation::Service, 2, 1, 11},
	    {cl::Operation::FieldRead, 1, 3, 704},
	    {cl::Operation::FieldWrite, 3, 0, 888},
	    {cl::Operation::FieldRead, 3, 0, 888},
	}};
	const std::array<std::uint32_t, 12> expectedValue{302, 0,    0,    2007, 700, 206,
	                                                  0,   3309, 2317, 704,  0,   888};
	std::uint32_t now = 0;
	cl::Clock clock{&now, +[](void* p) noexcept {
		                return *static_cast<std::uint32_t*>(p);
	                }};
	std::array<std::byte, 1024> tx{}, rx{}, port{}, reply{}, request{}, scratch{};
	unsigned completions = 0;
	en::EncodedBackend backend{tx, rx, clock, 30000, false, true};
	dy::DynamicClient<4, 1024, 1536> client{backend, clock};
	CHECK(client.admit(schema, schema.descriptor().first(64), 29) == cl::Status::Ok);
	const bt::wire::Capability capability{29, schema.fingerprint(), 32, 1024, 1024};
	const auto bound = bt::bindBatch(client, backend, capability, selected);
	CHECK(bound);
	std::array<std::array<std::byte, 4>, 7> arguments{};
	CHECK(ts::encode(std::uint32_t{700}, arguments[0]) == ts::CodecStatus::Ok);
	CHECK(ts::encode(Argument{5}, arguments[1]) == ts::CodecStatus::Ok);
	CHECK(ts::encode(Argument{7}, arguments[2]) == ts::CodecStatus::Ok);
	CHECK(ts::encode(Argument{4}, arguments[3]) == ts::CodecStatus::Ok);
	CHECK(ts::encode(Argument{9}, arguments[4]) == ts::CodecStatus::Ok);
	CHECK(ts::encode(Argument{11}, arguments[5]) == ts::CodecStatus::Ok);
	CHECK(ts::encode(std::uint32_t{888}, arguments[6]) == ts::CodecStatus::Ok);
	const std::array<cl::Input, 12> payloads{{{},
	                                          arguments[0],
	                                          arguments[1],
	                                          arguments[2],
	                                          {},
	                                          {},
	                                          arguments[3],
	                                          arguments[4],
	                                          arguments[5],
	                                          {},
	                                          arguments[6],
	                                          {}}};
	CHECK(bt::batch(
	    client, bound.binding, payloads, request, [&](const bt::BatchResult& result) noexcept {
		    ++completions;
		    CHECK(result.operation.succeeded() && result.valueStatus == bt::ValueStatus::Ok &&
		          result.count == expected.size());
		    for (std::size_t i = 0; i < expected.size(); ++i) {
			    const auto& item = result.items[i];
			    CHECK(item.id == ts::detail::makeId(expected[i].group, expected[i].index));
			    CHECK(item.operation.operation == expected[i].operation);
			    CHECK(item.operation.status == cl::Status::Ok && item.operation.dispatch == 0 &&
			          item.operation.endpointStatus == 0 && item.operation.succeeded());
			    if (expected[i].operation == cl::Operation::Command) {
				    CHECK(item.operation.executed() && !item.operation.accepted() &&
				          !item.hasValue());
			    } else if (expected[i].operation == cl::Operation::FieldWrite) {
				    CHECK(!item.hasValue());
			    } else {
				    CHECK(item.hasValue());
				    auto value = item.value;
				    if (expected[i].operation == cl::Operation::Service) {
					    const auto member = value.member("value");
					    CHECK(member);
					    value = member.value;
				    }
				    const auto scalar = value.get<std::uint32_t>();
				    CHECK(scalar && scalar.value == expectedValue[i]);
			    }
		    }
	    }));
	CHECK(client.pending() == 1 && completions == 0 && eventCount == 0);
	const auto packet = client.nextPacket();
	CHECK(packet && packet.mayExecute && packet.bytes.size() <= port.size());
	std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
	const auto frame = cl::Input{port}.first(packet.bytes.size());
	const auto envelope = ew::parse(frame);
	CHECK(envelope && envelope.header.operation == ew::Operation::Batch);
	const auto wire = bt::wire::parseRequest(envelope.payload);
	CHECK(wire && wire.count == expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i) {
		CHECK(wire.items[i].id == ts::detail::makeId(expected[i].group, expected[i].index));
		CHECK(static_cast<unsigned>(wire.items[i].operation) ==
		      static_cast<unsigned>(expected[i].operation));
		CHECK(std::equal(wire.items[i].payload.begin(), wire.items[i].payload.end(),
		                 payloads[i].begin(), payloads[i].end()));
	}
	CHECK(client.pending() == 1 && eventCount == 0);
	CHECK(client.prepareHandoff(packet.session, packet.requestId));
	CHECK(client.onSendAccepted(packet.session, packet.requestId));
	ts::Workspace workspace{scratch};
	const auto dispatched =
	    en::dispatchEncoded(model.view(), workspace, packet.session, frame, reply, capability);
	CHECK(dispatched && eventCount == expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i)
		CHECK(events[i] == expected[i]);
	CHECK(client.onReply(cl::Input{reply}.first(dispatched.written), packet.session,
	                     packet.requestId));
	CHECK(client.pending() == 0 && completions == 1);
	// Untouched rows, including every group-zero decoy, retain their own state.
	for (unsigned group = 0; group < 4; ++group)
		for (unsigned index = 0; index < 4; ++index) {
			const auto wanted = group == 1 && index == 3   ? 704u
			                    : group == 2 && index == 1 ? 206u
			                    : group == 3 && index == 0 ? 888u
			                                               : original[group].values[index];
			CHECK(devices[group].values[index] == wanted);
		}
	client.process(now);
	CHECK(!client.nextPacket() && completions == 1 && eventCount == expected.size());
	client.close();
	client.process(now);
	CHECK(client.drained() && completions == 1);
	std::printf("Batch CrossGroup checks=%u failures=0\n", checks);
}

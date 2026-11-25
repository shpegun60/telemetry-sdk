/* Four-surface SDK oracle: actual typed/runtime clients and the existing native Model. MIT. */
#include "../batch_static/Model.hpp"
#include <conformance_api/ClientBindings.hpp>
#include <integration/batch/StaticClient.hpp>
#include <integration/batch/Client.hpp>
#include <integration/read_many/StaticClient.hpp>
#include <integration/read_many/Client.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/BatchDispatcher.hpp>
#include <telemetry/runtime/DynamicValueWriter.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace native = static_batch_fixture;
namespace api = conformance_api;
namespace cl = telemetry::client;
namespace en = telemetry::integration::encoded;
namespace dy = telemetry::integration::dynamic;
namespace rm = telemetry::integration::read_many;
namespace bt = telemetry::integration::batch;
namespace rt = telemetry::runtime;
namespace ew = telemetry::protocol::encoded;
using Data = api::control::types::Config;
static unsigned checks = 0;

static void check(bool ok, unsigned line) noexcept
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "SDK conformance check=%u line=%u failed\n", checks, line);
		std::abort();
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

static std::string hex(cl::Input input)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	for (auto byte : input) {
		const auto value = std::to_integer<unsigned>(byte);
		result.push_back(digits[value >> 4]);
		result.push_back(digits[value & 15]);
	}
	return result;
}

template<class Status>
static const char* label(Status status) noexcept
{
	switch (status) {
		case Status::Ok:
			return "Ok";
		case Status::Stale:
			return "Stale";
		case Status::NoValue:
			return "NoValue";
		case Status::InvalidPayload:
			return "InvalidPayload";
		default:
			return "TypeMismatch";
	}
}

struct Item {
	std::uint32_t id;
	unsigned operation = 0, status = 0, dispatch = 0, endpointStatus = 0;
	bool succeeded = false, accepted = false, executed = false, hasValue = false;
	const char* valueStatus = nullptr;
};

struct Record {
	unsigned operation = 0, status = 0, dispatch = 0, endpointStatus = 0;
	bool succeeded = false, accepted = false, executed = false, hasValue = false;
	const char* valueStatus = nullptr;
	std::string request, reply, payload;
	std::vector<Item> items;

	void capture(const cl::ResultView& result, bool present, const char* values = nullptr)
	{
		operation = static_cast<unsigned>(result.operation);
		status = static_cast<unsigned>(result.status);
		dispatch = result.dispatch;
		endpointStatus = result.endpointStatus;
		succeeded = result.succeeded();
		accepted = result.accepted();
		executed = result.executed();
		hasValue = present;
		valueStatus = values;
		payload = hex(result.payload);
	}

	void item(std::uint32_t id, const cl::ResultView& outcome, bool present, const char* values)
	{
		items.push_back({id, static_cast<unsigned>(outcome.operation),
		                 static_cast<unsigned>(outcome.status), outcome.dispatch,
		                 outcome.endpointStatus, outcome.succeeded(), outcome.accepted(),
		                 outcome.executed(), present, values});
	}
};

struct Surface {
	const char* name;
	std::array<Record, 7> records{};
	std::array<native::Event, 128> trace{};
	unsigned count = 0, number = 0, reads = 0, writes = 0, commands = 0, services = 0;

	void finish() noexcept
	{
		trace = native::trace;
		count = native::traceCount;
		number = native::device.current.number;
		reads = native::device.reads;
		writes = native::device.writes;
		commands = native::device.commands;
		services = native::device.services;
		CHECK(native::decoy.reads == 0 && native::decoy.writes == 0 &&
		      native::decoy.commands == 0 && native::decoy.services == 0);
	}
};

struct Metadata {
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 32> members{};
	std::array<rt::RuntimeEnumEntry, 16> enums{};
	std::array<rt::RuntimeCatalog, 8> catalogs{};
	std::array<rt::RuntimeEndpoint, 40> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	rt::RuntimeSchema schema;

	Metadata() noexcept
	{
		const auto parsed = rt::RuntimeSchema::parse(native::descriptorBytes, workspace);
		CHECK(parsed);
		schema = parsed.schema;
	}
};

struct Port {
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* p) noexcept {
		                return static_cast<Port*>(p)->now;
	                }};
	std::array<std::byte, 8192> tx{}, rx{}, response{}, scratch{}, packet{};
	telemetry::Workspace workspace{scratch};
	en::EncodedBackend backend{tx, rx, clock, 30000, true, true};
	rm::wire::Capability trm{17, native::descriptor.fingerprint()};
	bt::wire::Capability tbm{17, native::descriptor.fingerprint(), 32, 8192, 8192};

	template<class Client, class Retire>
	void exchange(Client& client, Record& record, Retire retire)
	{
		auto offered = client.nextPacket();
		CHECK(offered);
		std::copy(offered.bytes.begin(), offered.bytes.end(), packet.begin());
		offered.bytes = cl::Input{packet}.first(offered.bytes.size());
		record.request = hex(offered.bytes);
		CHECK(client.prepareHandoff(offered.session, offered.requestId));
		CHECK(client.onSendAccepted(offered.session, offered.requestId));
		const auto parsed = ew::parse(offered.bytes);
		CHECK(parsed);
		const auto reply = parsed.header.operation == ew::Operation::ReadMany
		                       ? en::dispatchEncoded(native::model.view(), workspace, 17,
		                                             offered.bytes, response, trm)
		                       : en::dispatchEncoded(native::model.view(), workspace, 17,
		                                             offered.bytes, response, tbm);
		CHECK(reply);
		record.reply = hex(cl::Input{response}.first(reply.written));
		retire();
		CHECK(client.onReply(cl::Input{response}.first(reply.written), offered.session,
		                     offered.requestId));
		client.process(now);
	}

	template<class Client>
	void exchange(Client& client, Record& record)
	{
		exchange(client, record, []() noexcept {
		});
	}
};

static Data data(std::uint32_t number) noexcept
{
	return {
	    number,     true,      {0, 65535, 17}, static_cast<decltype(Data::mode)>(-123),
	    UINT64_MAX, INT64_MIN, 4.25f,          std::bit_cast<double>(UINT64_C(0x8000000000000000))};
}

static void exact(const Data& value, std::uint32_t number) noexcept
{
	CHECK(value.number_value == number && value.enabled);
	CHECK((value.gains == std::array<std::uint16_t, 3>{0, 65535, 17}));
	CHECK(static_cast<std::int16_t>(value.mode) == -123);
	CHECK(value.large == UINT64_MAX && value.negative == INT64_MIN);
	CHECK(std::bit_cast<std::uint32_t>(value.f32) == 0x40880000);
	CHECK(std::bit_cast<std::uint64_t>(value.f64) == UINT64_C(0x8000000000000000));
}

static std::uint32_t number(rt::DynamicValueView value) noexcept
{
	const auto member = value.member("number");
	CHECK(member);
	const auto result = member.value.get<std::uint32_t>();
	CHECK(result);
	return result.value;
}

static std::array<std::byte, 41> runtimeData(const rt::RuntimeSchema& schema,
                                             std::uint32_t number) noexcept
{
	// This fixture's all-zero representation is canonical, checked by begin(); it is not a device default.
	std::array<std::byte, 41> seed{}, staging{}, output{};
	auto writer = rt::DynamicValueWriter::begin(schema, 14, seed, staging);
	CHECK(writer);
	auto set = [&](const char* path, auto value) noexcept {
		const auto handle = schema.resolve(14, path);
		CHECK(handle);
		CHECK(writer.set(handle.value, value) == rt::Status::Ok);
	};
	set("number", number);
	set("enabled", true);
	set("gains[0]", std::uint16_t{0});
	set("gains[1]", std::uint16_t{65535});
	set("gains[2]", std::uint16_t{17});
	const auto mode = schema.resolve(14, "mode");
	CHECK(mode);
	CHECK(writer.setEnumUnderlying(mode.value, std::int16_t{-123}) == rt::Status::Ok);
	set("large", UINT64_MAX);
	set("negative", INT64_MIN);
	set("f32", 4.25f);
	set("f64", std::bit_cast<double>(UINT64_C(0x8000000000000000)));
	CHECK(writer.commit(output));
	return output;
}

static Surface staticSequence()
{
	native::reset();
	Port port;
	Surface surface{"cpp-static"};
	cl::DeviceClient<api::client::CodecPolicy, 4, 4096, 256> client{port.backend, port.clock};
	CHECK(client.admitDescriptor(api::client::CodecPolicy::descriptorHeader(), 17) ==
	      cl::Status::Ok);
	const auto written = data(1111), configured = data(37), echoed = data(99),
	           batchValue = data(888), final = data(55);
	CHECK(client.write(api::control::Config, written, [&](const auto& result) noexcept {
		surface.records[0].capture(result, false);
	}));
	port.exchange(client, surface.records[0]);
	CHECK(client.read(api::control::Config, [&](const auto& result) noexcept {
		CHECK(result.hasValue());
		exact(result.value(), 1000);
		surface.records[1].capture(result, true);
	}));
	port.exchange(client, surface.records[1]);
	CHECK(client.command(api::control::Configure, configured, [&](const auto& result) noexcept {
		CHECK(result.executed());
		surface.records[2].capture(result, false);
	}));
	port.exchange(client, surface.records[2]);
	CHECK(client.service(api::control::Echo, echoed, [&](const auto& result) noexcept {
		CHECK(result.hasValue());
		exact(result.value(), 99);
		surface.records[3].capture(result, true);
	}));
	port.exchange(client, surface.records[3]);
	const auto watch = rm::bindReadMany(client, port.backend, port.trm,
	                                    rm::fields(api::control::Counter, api::control::Bool));
	CHECK(watch);
	CHECK(rm::readMany(client, watch.binding, [&](const auto& result) noexcept {
		auto& record = surface.records[4];
		record.capture(result.operation, false, label(result.valueStatus));
		CHECK(result.template get<0>().hasValue() && result.template get<0>().value() == 112);
		CHECK(result.template get<1>().hasValue() && result.template get<1>().value());
		[&]<std::size_t... I>(std::index_sequence<I...>) {
			(record.items.push_back({result.template get<I>().id, 0,
			                         static_cast<unsigned>(result.template get<I>().status), 0, 0,
			                         false, false, false, result.template get<I>().hasValue(),
			                         label(result.template get<I>().valueStatus)}),
			 ...);
		}(std::make_index_sequence<2>{});
		record.hasValue =
		    std::any_of(record.items.begin(), record.items.end(), [](const auto& item) {
			    return item.hasValue;
		    });
	}));
	port.exchange(client, surface.records[4]);
	const auto request =
	    bt::items(bt::write(api::control::Config, batchValue), bt::read(api::control::Config),
	              bt::command(api::control::Accepted, configured),
	              bt::service(api::control::Busy, echoed), bt::service(api::control::Echo, final));
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	const auto batch = bt::bindBatch(client, port.backend, port.tbm, request);
	CHECK(batch);
	auto complete = [&](auto& record, const auto& result) noexcept {
		record.capture(result.operation, false, label(result.valueStatus));
		[&]<std::size_t... I>(std::index_sequence<I...>) {
			(
			    [&] {
				    const auto item = result.template get<I>();
				    CHECK(item);
				    constexpr std::array<std::uint32_t, 5> ids{65536, 65536, 65538, 65539, 65536};
				    record.item(ids[I], item->operation, item->hasValue(),
				                label(item->valueStatus));
			    }(),
			    ...);
		}(std::make_index_sequence<5>{});
		record.hasValue =
		    std::any_of(record.items.begin(), record.items.end(), [](const auto& item) {
			    return item.hasValue;
		    });
	};
	CHECK(bt::batch(client, batch.binding, request, storage, [&](const auto& result) noexcept {
		complete(surface.records[5], result);
		exact(result.template get<1>()->value(), 888);
		exact(result.template get<4>()->value(), 55);
	}));
	port.exchange(client, surface.records[5]);
	CHECK(bt::batch(client, batch.binding, request, storage, [&](const auto& result) noexcept {
		complete(surface.records[6], result);
		CHECK(result.valueStatus == bt::StaticValueStatus::Stale);
		for (const auto& item : surface.records[6].items)
			CHECK(!item.hasValue);
	}));
	port.exchange(client, surface.records[6], [&]() noexcept {
		client.invalidateAdmission();
	});
	client.close();
	client.process(port.now);
	CHECK(client.drained());
	surface.finish();
	return surface;
}

static Surface dynamicSequence()
{
	native::reset();
	Metadata metadata;
	Port port;
	Surface surface{"cpp-dynamic"};
	dy::DynamicClient<4, 4096, 2048> client{port.backend, port.clock};
	CHECK(client.admit(metadata.schema, cl::Input{native::descriptorBytes}.first(64), 17) ==
	      cl::Status::Ok);
	auto bind = [&](cl::Operation operation, std::uint32_t id = 65536) noexcept {
		const auto bound = client.bind(operation, id);
		CHECK(bound);
		return bound.endpoint;
	};
	const auto write = bind(cl::Operation::FieldWrite), read = bind(cl::Operation::FieldRead);
	const auto command = bind(cl::Operation::Command), service = bind(cl::Operation::Service);
	const auto written = runtimeData(metadata.schema, 1111),
	           configured = runtimeData(metadata.schema, 37);
	const auto echoed = runtimeData(metadata.schema, 99),
	           batchValue = runtimeData(metadata.schema, 888),
	           final = runtimeData(metadata.schema, 55);
	auto submit = [&](unsigned index, const auto& endpoint, cl::Input payload) {
		CHECK(
		    client.submit(endpoint, payload, [&, index](const dy::DynamicResult& result) noexcept {
			    surface.records[index].capture(result.operation, result.hasValue(),
			                                   label(result.valueStatus));
			    if (result.hasValue())
				    CHECK(number(result.value) == (index == 1 ? 1000u : 99u));
		    }));
		port.exchange(client, surface.records[index]);
	};
	submit(0, write, written);
	submit(1, read, {});
	submit(2, command, configured);
	submit(3, service, echoed);
	const std::array<std::uint32_t, 2> ids{65537, 65540};
	const auto watch = rm::bindReadMany(client, port.backend, port.trm, ids);
	CHECK(watch);
	CHECK(rm::readMany(client, watch.binding, [&](const rm::ReadManyResult& result) noexcept {
		auto& record = surface.records[4];
		record.capture(result.operation, false, label(result.valueStatus));
		CHECK(result.count == 2 && result.items[0].hasValue() && result.items[1].hasValue());
		const auto counter = result.items[0].value.get<std::uint32_t>();
		CHECK(counter && counter.value == 112);
		const auto flag = result.items[1].value.get<bool>();
		CHECK(flag && flag.value);
		for (unsigned i = 0; i < result.count; ++i) {
			const auto& item = result.items[i];
			record.items.push_back({item.id, 0, static_cast<unsigned>(item.status), 0, 0, false,
			                        false, false, item.hasValue(), label(item.valueStatus)});
		}
		record.hasValue =
		    std::any_of(record.items.begin(), record.items.end(), [](const auto& item) {
			    return item.hasValue;
		    });
	}));
	port.exchange(client, surface.records[4]);
	const std::array<bt::Endpoint, 5> endpoints{{{cl::Operation::FieldWrite, 65536},
	                                             {cl::Operation::FieldRead, 65536},
	                                             {cl::Operation::Command, 65538},
	                                             {cl::Operation::Service, 65539},
	                                             {cl::Operation::Service, 65536}}};
	const auto batch = bt::bindBatch(client, port.backend, port.tbm, endpoints);
	CHECK(batch);
	const std::array<cl::Input, 5> payloads{batchValue, {}, configured, echoed, final};
	std::array<std::byte, 8192> request{};
	auto complete = [&](auto& record, const bt::BatchResult& result) noexcept {
		record.capture(result.operation, false, label(result.valueStatus));
		CHECK(result.count == 5);
		for (unsigned i = 0; i < result.count; ++i) {
			const auto& item = result.items[i];
			record.item(item.id, item.operation, item.hasValue(), label(item.valueStatus));
		}
		record.hasValue =
		    std::any_of(record.items.begin(), record.items.end(), [](const auto& item) {
			    return item.hasValue;
		    });
	};
	CHECK(bt::batch(
	    client, batch.binding, payloads, request, [&](const bt::BatchResult& result) noexcept {
		    complete(surface.records[5], result);
		    CHECK(number(result.items[1].value) == 888 && number(result.items[4].value) == 55);
	    }));
	port.exchange(client, surface.records[5]);
	CHECK(bt::batch(client, batch.binding, payloads, request,
	                [&](const bt::BatchResult& result) noexcept {
		                complete(surface.records[6], result);
		                CHECK(result.valueStatus == bt::ValueStatus::Stale);
		                for (const auto& item : surface.records[6].items)
			                CHECK(!item.hasValue);
	                }));
	port.exchange(client, surface.records[6], [&]() noexcept {
		client.invalidate();
	});
	client.close();
	client.process(port.now);
	CHECK(client.drained());
	surface.finish();
	return surface;
}

static const char* boolean(bool value) noexcept
{
	return value ? "true" : "false";
}

static void facts(std::FILE* out, const auto& value)
{
	std::fprintf(
	    out,
	    "\"operation\":%u,\"status\":%u,\"dispatch\":%u,\"endpointStatus\":%u,"
	    "\"succeeded\":%s,\"accepted\":%s,\"executed\":%s,\"hasValue\":%s,\"valueStatus\":",
	    value.operation, value.status, value.dispatch, value.endpointStatus,
	    boolean(value.succeeded), boolean(value.accepted), boolean(value.executed),
	    boolean(value.hasValue));
	if (value.valueStatus)
		std::fprintf(out, "\"%s\"", value.valueStatus);
	else
		std::fputs("null", out);
}

static void emit(std::FILE* out, const Surface& surface)
{
	std::fprintf(out, "{\"name\":\"%s\",\"records\":[", surface.name);
	for (unsigned i = 0; i < surface.records.size(); ++i) {
		const auto& record = surface.records[i];
		std::fputs(i ? ",{" : "{", out);
		facts(out, record);
		std::fprintf(out, ",\"request\":\"%s\",\"reply\":\"%s\",\"payload\":\"%s\",\"items\":[",
		             record.request.c_str(), record.reply.c_str(), record.payload.c_str());
		for (unsigned n = 0; n < record.items.size(); ++n) {
			const auto& item = record.items[n];
			std::fprintf(out, "%s{\"id\":%u,", n ? "," : "", item.id);
			if (item.operation)
				facts(out, item);
			else
				std::fprintf(out, "\"status\":%u,\"hasValue\":%s,\"valueStatus\":\"%s\"",
				             item.status, boolean(item.hasValue), item.valueStatus);
			std::fputc('}', out);
		}
		std::fputs("]}", out);
	}
	std::fprintf(
	    out,
	    "],\"state\":{\"number\":%u,\"reads\":%u,\"writes\":%u,\"commands\":%u,\"services\":%u,\"trace\":[",
	    surface.number, surface.reads, surface.writes, surface.commands, surface.services);
	for (unsigned i = 0; i < surface.count; ++i) {
		const auto& event = surface.trace[i];
		std::fprintf(out, "%s[%u,%u,%u,%u]", i ? "," : "", event.operation, event.group,
		             event.index, event.number);
	}
	std::fputs("]}}", out);
}

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	const auto first = staticSequence(), second = dynamicSequence();
	for (unsigned i = 0; i < first.records.size(); ++i) {
		CHECK(first.records[i].request == second.records[i].request);
		CHECK(first.records[i].reply == second.records[i].reply);
	}
	CHECK(first.count == second.count && first.number == second.number);
	for (unsigned i = 0; i < first.count; ++i)
		CHECK(first.trace[i] == second.trace[i]);
	auto* output = std::fopen(argv[1], "wb");
	if (!output)
		return 3;
	std::fputs("{\"surfaces\":[", output);
	emit(output, first);
	std::fputc(',', output);
	emit(output, second);
	std::fputs("]}\n", output);
	if (std::fclose(output) != 0)
		return 3;
	std::printf("SDK native conformance checks=%u failures=0\n", checks);
	return 0;
}

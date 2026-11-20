/*
 * @file Parity.cpp
 * @brief Generated (Static) and Dynamic endpoint handles, by path, against one native Model.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The group-tree fixture of tests/structured/group_tree runs in process behind
 * two Encoded backends: the generated tree_api::client::Device over the
 * generated DeviceClient, and DynamicClient::endpoint(path) over RuntimeSchema.
 * The same paths give the same values and the same refusals; the nested form,
 * device.endpoint<"...">() and tree_api::client::endpoint<"...">(device) give
 * the same handle. Refusals the Static device makes at build time are the
 * negative compile probes of run.py; this program prints the Dynamic wording
 * (describePath) for run.py to compare with both JS clients.
 */

#include "../structured/group_tree/AdcBranch.hpp"
#include "../structured/group_tree/MetrologyBranch.hpp"

#include <tree_api/ClientBindings.hpp>

#include <integration/dynamic/DynamicClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <telemetry/descriptor/Descriptor.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
namespace ts = ::telemetry;
namespace cl = ::telemetry::client;
namespace rt = ::telemetry::runtime;
namespace dy = ::telemetry::integration::dynamic;
namespace en = ::telemetry::integration::encoded;
namespace api = ::tree_api;
using Bytes = std::vector<std::byte>;

inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};
inline constexpr ts::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = ts::descriptor::packDescriptor<descriptor>();

int checks = 0, failures = 0;

void check(bool condition, const char* what) noexcept
{
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAIL %s\n", what);
	}
}

std::uint32_t now = 0;
const cl::Clock clock{nullptr, +[](void*) noexcept {
	                      return now;
                      }};
std::array<std::byte, 256> scratch{};
ts::Workspace workspace{scratch};

template<class Client>
void pump(Client& client) noexcept
{
	std::array<std::byte, 256> port{}, reply{};
	for (unsigned i = 0; i < 16; ++i) {
		auto packet = client.nextPacket();
		if (!packet)
			break;
		std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
		const auto request = cl::Input{port}.first(packet.bytes.size());
		check(client.prepareHandoff(packet.session, packet.requestId), "handoff");
		check(client.onSendAccepted(packet.session, packet.requestId), "send accepted");
		const auto r = en::dispatchEncoded(model.view(), workspace, packet.session, request, reply);
		check(r.status == en::EncodedDispatchStatus::Ok, "native dispatch");
		check(client.onReply(cl::Input{reply}.first(r.written), packet.session, packet.requestId),
		      "reply");
		client.process(now);
	}
}

template<class T>
Bytes encoded(const T& value) noexcept
{
	Bytes bytes(api::codec::wireSize<T>);
	check(api::codec::encode(value, bytes) == api::codec::Status::Ok, "generated encode");
	return bytes;
}

Bytes copied(cl::Input bytes)
{
	return Bytes(bytes.begin(), bytes.end());
}

struct Harness {
	std::array<std::byte, 1024> tx1{}, rx1{}, tx2{}, rx2{};
	en::EncodedBackend staticBackend{tx1, rx1, clock};
	en::EncodedBackend dynamicBackend{tx2, rx2, clock};
	api::client::DeviceClient<> staticClient{staticBackend, clock};
	api::client::Device<api::client::DeviceClient<>> device{staticClient};
	std::array<rt::RuntimeType, 64> types{};
	std::array<rt::RuntimeMember, 64> members{};
	std::array<rt::RuntimeEnumEntry, 64> enums{};
	std::array<rt::RuntimeCatalog, 64> catalogs{};
	std::array<rt::RuntimeEndpoint, 64> endpoints{};
	rt::RuntimeSchemaWorkspace schemaWorkspace{types, members, enums, catalogs, endpoints};
	dy::DynamicClient<8, 4096> dynamicClient{dynamicBackend, clock};
	rt::RuntimeSchema schema{};

	Harness() noexcept
	{
		const auto bytes = cl::Input{reinterpret_cast<const std::byte*>(descriptorBytes.data()),
		                             descriptorBytes.size()};
		check(api::schema::matchesHeader(bytes.first(64)), "the generated SDK is this Model's");
		const auto parsed = rt::RuntimeSchema::parse(bytes, schemaWorkspace);
		check(static_cast<bool>(parsed), "RuntimeSchema parses the Model's descriptor");
		schema = parsed.schema;
		check(staticClient.admitDescriptor(api::schema::descriptorHeader, 17) == cl::Status::Ok,
		      "static admission");
		check(dynamicClient.admit(schema, bytes.first(64), 17) == cl::Status::Ok,
		      "dynamic admission");
	}

	dy::DynamicClient<8, 4096>::PathHandle handle(std::string_view path) noexcept
	{
		const auto result = dynamicClient.endpoint(path);
		check(static_cast<bool>(result), "dynamic endpoint(path)");
		return result.handle;
	}

	// The whole value of a Field through the Dynamic handle, as wire bytes.
	Bytes dynamicRead(std::string_view path) noexcept
	{
		Bytes bytes;
		bool done = false;
		const auto submitted = handle(path).read([&](const dy::DynamicResult& r) noexcept {
			done = true;
			check(r.operation.succeeded() && r.hasValue(), "dynamic read succeeds");
			if (const auto view = r.value.bytes())
				bytes = copied(view.value);
		});
		check(submitted.status == cl::Status::Ok, "dynamic read submitted");
		pump(dynamicClient);
		check(done, "dynamic read completed");
		return bytes;
	}

	template<api::client::handles::Path P>
	Bytes staticRead() noexcept
	{
		Bytes bytes;
		bool done = false;
		const auto submitted = device.read<P>([&](const auto& r) noexcept {
			done = true;
			check(r.succeeded(), "static read succeeds");
			bytes = encoded(r.value());
		});
		check(submitted.status == cl::Status::Ok, "static read submitted");
		pump(staticClient);
		check(done, "static read completed");
		return bytes;
	}

	template<api::client::handles::Path P>
	void compareRead() noexcept
	{
		const auto expected = staticRead<P>();
		check(!expected.empty() && expected == dynamicRead(P.view()),
		      "a Field reads the same through both clients");
		std::printf("read %.*s %zu bytes\n", static_cast<int>(P.view().size()), P.view().data(),
		            expected.size());
	}

	// A member path reads the same bytes as the matching part of the whole Field.
	void compareMember(std::string_view whole, std::string_view path) noexcept
	{
		const auto field = dynamicRead(whole);
		const auto target = schema.resolvePath(path);
		check(static_cast<bool>(target), "RuntimeSchema resolves the member path");
		Bytes part;
		const auto member = handle(path);
		check(member.member(), "a member handle");
		const auto submitted = member.read([&](const dy::DynamicResult& r) noexcept {
			check(r.hasValue(), "member value");
			if (const auto view = r.value.bytes())
				part = copied(view.value);
		});
		check(submitted.status == cl::Status::Ok, "member read submitted");
		pump(dynamicClient);
		const auto offset = target.value.handle.wireOffset();
		check(!part.empty() && offset + part.size() <= field.size() &&
		          std::equal(part.begin(), part.end(), field.begin() + offset),
		      "a member read is the member's part of the Field");
		std::printf("member %.*s %zu bytes\n", static_cast<int>(path.size()), path.data(),
		            part.size());
	}

	void refusal(std::string_view path, std::string_view method, dy::PathStatus status,
	             std::string_view kind) noexcept
	{
		std::array<char, 256> text{};
		const auto size = dy::describePath(status, path, kind, method, text);
		std::printf("error %.*s %.*s %.*s\n", static_cast<int>(path.size()), path.data(),
		            static_cast<int>(method.size()), method.data(), static_cast<int>(size),
		            text.data());
	}
};

} // namespace

int main()
{
	Harness h;
	auto& device = h.device;

	// One handle, three spellings.
	static_assert(std::is_same_v<decltype(device.endpoint<"adc/channel1/Gain">()),
	                             decltype(device.adc.channel1.Gain)>);
	check(device.endpoint<"adc/channel1/Gain">() == device.adc.channel1.Gain,
	      "endpoint<path>() is the nested handle");
	check(api::client::endpoint<"adc/Config">(device) == device.adc.Config,
	      "api::client::endpoint<path>(device) is the nested handle");
	check(!(device.adc.channel1.Gain == device.adc.channel2.Gain), "distinct endpoints differ");
	auto gain1 = device.adc.channel1.Gain; // a handle kept in a variable
	check(gain1.id() == ts::idOf<model, "adc/channel1/Gain">() &&
	          device.adc.Calibrate.id() == ts::idOf<model, "adc/Calibrate">() &&
	          device.metrology.Config.id() == ts::idOf<model, "metrology/Config">(),
	      "handles carry the device's PackedIds");
	static_assert(std::is_same_v<decltype(gain1)::Value, float>);
	static_assert(std::is_same_v<decltype(device.adc.Config)::Value, api::types::adc::Config>);

	// Values: every Field, then members.
	h.compareRead<"adc/Config">();
	h.compareRead<"adc/Status">();
	h.compareRead<"adc/channel1/Gain">();
	h.compareRead<"adc/channel2/Gain">();
	h.compareRead<"metrology/Config">();
	h.compareMember("adc/Config", "adc/Config.rateHz");
	h.compareMember("adc/Status", "adc/Status.raw[1]");
	h.compareMember("metrology/Config", "metrology/Config.window");

	// A write through one client reads back through the other.
	bool written = false;
	check(gain1.write(4.5f,
	                  [&](const auto& r) noexcept {
		                  written = r.succeeded();
	                  })
	              .status == cl::Status::Ok,
	      "static handle write");
	pump(h.staticClient);
	const auto gain = h.dynamicRead("adc/channel1/Gain");
	check(written && gain == encoded(4.5f), "a static write reads back dynamically");
	const auto config = encoded(api::types::adc::Config{0.25f, 2000, false});
	written = false;
	check(h.handle("adc/Config")
	              .write(config,
	                     [&](const dy::DynamicResult& r) noexcept {
		                     written = r.operation.succeeded();
	                     })
	              .status == cl::Status::Ok,
	      "dynamic handle write");
	pump(h.dynamicClient);
	check(written && h.staticRead<"adc/Config">() == config,
	      "a dynamic write reads back statically");

	// Commands and Services.
	const auto resets = adc::tlm::instance.resets;
	bool executed = false;
	check(device.call<"adc/Reset">([&](const auto& r) noexcept {
		            executed = r.executed();
	            }).status == cl::Status::Ok,
	      "static call of a Command");
	pump(h.staticClient);
	check(h.handle("adc/Reset")
	              .call([&](const dy::DynamicResult& r) noexcept {
		              executed = executed && r.operation.executed();
	              })
	              .status == cl::Status::Ok,
	      "dynamic call of a Command");
	pump(h.dynamicClient);
	check(executed && adc::tlm::instance.resets == resets + 2, "both Reset calls executed");
	const api::types::adc::Reference reference{2.0f};
	Bytes staticOffset, dynamicOffset;
	check(device.call<"adc/Calibrate">(reference,
	                                   [&](const auto& r) noexcept {
		                                   if (r.hasValue())
			                                   staticOffset = encoded(r.value());
	                                   })
	              .status == cl::Status::Ok,
	      "static call of a Service");
	pump(h.staticClient);
	check(h.handle("adc/Calibrate")
	              .call(encoded(reference),
	                    [&](const dy::DynamicResult& r) noexcept {
		                    if (const auto view = r.value.bytes())
			                    dynamicOffset = copied(view.value);
	                    })
	              .status == cl::Status::Ok,
	      "dynamic call of a Service");
	pump(h.dynamicClient);
	check(!staticOffset.empty() && staticOffset == dynamicOffset &&
	          staticOffset == encoded(api::types::adc::Offset{4.0f}),
	      "a Service answers the same through both clients");

	// A read-only write: both clients refuse it before any packet, with ReadOnly.
	const auto status = h.staticRead<"adc/Status">();
	check(device.adc.Status_value.write(api::types::adc::Status{}).status == cl::Status::ReadOnly,
	      "the static client refuses a read-only write");
	check(h.handle("adc/Status")
	              .write(status,
	                     [](const dy::DynamicResult&) noexcept {
	                     })
	              .status == cl::Status::ReadOnly,
	      "the dynamic handle refuses a read-only write");

	// Refusals of the Dynamic client, in the wording both JS clients throw.
	using dy::PathStatus;
	const auto refused = [&](std::string_view path, PathStatus expected,
	                         std::string_view kind = "Field") {
		const auto result = h.dynamicClient.endpoint(path);
		check(result.status == expected, "endpoint(path) refuses for the expected reason");
		h.refusal(path, "endpoint", result.status, kind);
	};
	refused("adc/channel9/Gain", PathStatus::NoEndpoint);
	refused("adc/Config.nope", PathStatus::NoMember);
	refused("adc/Status.raw[5]", PathStatus::NoMember);
	refused("adc/Calibrate.volts", PathStatus::MemberOfCall, "Service");
	const auto operation = [&](std::string_view path, cl::Operation op, std::string_view method,
	                           PathStatus expected) {
		const auto handle = h.handle(path);
		check(handle.accepts(op) == expected, "a handle refuses for the expected reason");
		h.refusal(path, method, handle.accepts(op), dy::kindName(handle.category()));
	};
	operation("adc/Reset", cl::Operation::FieldRead, "read", PathStatus::WrongKind);
	operation("adc/Calibrate", cl::Operation::FieldWrite, "write", PathStatus::WrongKind);
	operation("adc/Config", cl::Operation::Command, "call", PathStatus::WrongKind);
	operation("adc/Status", cl::Operation::FieldWrite, "write", PathStatus::ReadOnly);
	operation("adc/Config.rateHz", cl::Operation::FieldWrite, "write", PathStatus::MemberWrite);
	check(h.handle("adc/Reset")
	              .read([](const dy::DynamicResult&) noexcept {
	              })
	              .status == cl::Status::InvalidArgument,
	      "a wrong-kind read is refused before any packet");

	// A handle outlives nothing it should not: after invalidation it refuses.
	const auto kept = h.handle("adc/channel2/Gain");
	h.dynamicClient.invalidate();
	check(kept.read([](const dy::DynamicResult&) noexcept {
	          }).status != cl::Status::Ok,
	      "a dynamic handle refuses after its admission ends");
	check(h.dynamicClient.endpoint("adc/Config").status == PathStatus::NotAdmitted,
	      "endpoint(path) needs an admission");
	std::printf("handles parity checks=%d failures=%d\n", checks, failures);
	return failures == 0 ? 0 : 1;
}

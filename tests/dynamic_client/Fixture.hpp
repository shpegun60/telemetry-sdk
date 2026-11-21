/* Actual Host Model and fixed metadata storage for DynamicClient checks. MIT. */
#ifndef TELEMETRY_TEST_DYNAMIC_CLIENT_FIXTURE_HPP
#define TELEMETRY_TEST_DYNAMIC_CLIENT_FIXTURE_HPP
#pragma once
#include <integration/dynamic/DynamicClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/RetryingBackend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <integration/rpc/Backend.hpp>
#include <integration/telemetry/v3/RpcSessionPool.hpp>
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dynamic_fixture {
namespace cl = telemetry::client;
namespace rt = telemetry::runtime;
namespace dy = telemetry::integration::dynamic;
namespace en = telemetry::integration::encoded;
namespace rpc = resource::telemetry::v3;
inline unsigned checks = 0;

inline void check(bool ok, unsigned line) noexcept
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "dynamic client check %u failed line %u\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) ::dynamic_fixture::check(bool(__VA_ARGS__), __LINE__)

struct Config {
	std::uint32_t value;
	bool enabled;
};

struct Empty {};

struct Big {
	std::array<std::uint32_t, 32> words;
};

struct Device {
	Config current{42, true};
	unsigned reads = 0, writes = 0, commands = 0, services = 0;

	Config read() noexcept
	{
		++reads;
		return current;
	}

	telemetry::WriteResult write(const Config& value) noexcept
	{
		++writes;
		current = value;
		return telemetry::WriteResult::Applied;
	}

	Empty empty() noexcept
	{
		++reads;
		return {};
	}

	telemetry::CommandResult configure(const Config& value) noexcept
	{
		++commands;
		current = value;
		return telemetry::CommandResult::Executed;
	}

	telemetry::CommandResult accepted(const Config&) noexcept
	{
		++commands;
		return telemetry::CommandResult::Accepted;
	}

	telemetry::CommandResult reset() noexcept
	{
		++commands;
		return telemetry::CommandResult::Executed;
	}

	telemetry::CommandResult big(const Big&) noexcept
	{
		++commands;
		return telemetry::CommandResult::Executed;
	}

	telemetry::ServiceResult<Config> echo(const Config& value) noexcept
	{
		++services;
		return telemetry::ServiceResult<Config>::success(value);
	}

	telemetry::ServiceResult<void> ping() noexcept
	{
		++services;
		return telemetry::ServiceResult<void>::success();
	}

	telemetry::ServiceResult<Empty> emptyService() noexcept
	{
		++services;
		return telemetry::ServiceResult<Empty>::success({});
	}

	telemetry::ServiceResult<Config> busy(const Config&) noexcept
	{
		++services;
		return telemetry::ServiceResult<Config>::failure(telemetry::ServiceStatus::Busy);
	}
};

inline Device device;
inline constexpr telemetry::Model model{
    telemetry::group("device", telemetry::field<&Device::read, &Device::write>("Config", device),
                     telemetry::field<&Device::read>("ReadOnly", device),
                     telemetry::field<&Device::empty>("Empty", device),
                     telemetry::command<&Device::configure>("Configure", device),
                     telemetry::command<&Device::accepted>("Accepted", device),
                     telemetry::command<&Device::reset>("Reset", device),
                     telemetry::command<&Device::big>("Big", device),
                     telemetry::service<&Device::echo>("Echo", device),
                     telemetry::service<&Device::ping>("Ping", device),
                     telemetry::service<&Device::emptyService>("EmptyService", device),
                     telemetry::service<&Device::busy>("Busy", device))};
inline constexpr auto modelView = model.view();
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = telemetry::descriptor::packDescriptor<descriptor>();

struct Metadata {
	std::array<rt::RuntimeType, 64> types{};
	std::array<rt::RuntimeMember, 64> members{};
	std::array<rt::RuntimeEnumEntry, 8> enums{};
	std::array<rt::RuntimeCatalog, 8> catalogs{};
	std::array<rt::RuntimeEndpoint, 32> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	rt::RuntimeSchema schema{};

	Metadata() noexcept
	{
		auto p = rt::RuntimeSchema::parse(bytes, workspace);
		CHECK(p);
		schema = p.schema;
	}
};

inline auto config(std::uint32_t value = 73, bool enabled = false) noexcept
{
	std::array<std::byte, 5> out{};
	for (unsigned i = 0; i < 4; ++i)
		out[i] = std::byte((value >> (i * 8)) & 255);
	out[4] = std::byte(enabled ? 1 : 0);
	return out;
}

struct Observed {
	unsigned calls = 0;
	cl::Status status = cl::Status::Busy;
	dy::ValueStatus valueStatus = dy::ValueStatus::NoValue;
	bool hasValue = false, accepted = false, executed = false;
	std::uint32_t number = 0;
	std::array<std::byte, 512> snapshot{};
	std::size_t size = 0;

	void operator()(const dy::DynamicResult& r) noexcept
	{
		++calls;
		status = r.operation.status;
		valueStatus = r.valueStatus;
		hasValue = r.hasValue();
		accepted = r.operation.accepted();
		executed = r.operation.executed();
		size = r.operation.payload.size();
		CHECK(size <= snapshot.size());
		std::copy(r.operation.payload.begin(), r.operation.payload.end(), snapshot.begin());
		if (hasValue && size == 5) {
			const auto member = r.value.member("value");
			CHECK(member);
			const auto v = member.value.get<std::uint32_t>();
			CHECK(v);
			number = v.value;
		}
	}
};

template<class Backend = en::EncodedBackend>
struct Encoded {
	Metadata metadata;
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* p) noexcept {
		                return static_cast<Encoded*>(p)->now;
	                }};
	std::array<std::byte, 1024> tx{}, rx{}, port{}, reply{}, scratch{};
	telemetry::Workspace workspace{scratch};
	Backend backend{tx, rx, clock};
	dy::DynamicClient<8, 4096> client{backend, clock};

	Encoded() noexcept
	{
		device = {};
		CHECK(client.admit(metadata.schema, cl::Input{bytes}.first(64), 17) == cl::Status::Ok);
	}

	auto bind(cl::Operation op, std::uint32_t id) noexcept
	{
		auto b = client.bind(op, id);
		CHECK(b);
		return b.endpoint;
	}

	cl::Packet offered() noexcept
	{
		auto p = client.nextPacket();
		CHECK(p);
		CHECK(p.bytes.size() <= port.size());
		std::copy(p.bytes.begin(), p.bytes.end(), port.begin());
		p.bytes = cl::Input{port}.first(p.bytes.size());
		return p;
	}

	void send(cl::Packet p, bool deliver = true) noexcept
	{
		CHECK(client.prepareHandoff(p.session, p.requestId));
		CHECK(client.onSendAccepted(p.session, p.requestId));
		const auto r = en::dispatchEncoded(model.view(), workspace, p.session, p.bytes, reply);
		CHECK(r.status == en::EncodedDispatchStatus::Ok);
		if (deliver)
			CHECK(client.onReply(cl::Input{reply}.first(r.written), p.session, p.requestId));
		client.process(now);
	}

	void pump() noexcept
	{
		for (unsigned i = 0; i < 32 && client.pending(); ++i) {
			auto p = offered();
			send(p);
		}
		CHECK(client.pending() == 0);
	}
};
} // namespace dynamic_fixture
#endif // TELEMETRY_TEST_DYNAMIC_CLIENT_FIXTURE_HPP

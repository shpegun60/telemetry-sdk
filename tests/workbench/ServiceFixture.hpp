/* Real Service request/response models sharing existing transport fixtures. MIT. */
#ifndef TELEMETRY_TEST_WORKBENCH_SERVICE_FIXTURE_HPP
#define TELEMETRY_TEST_WORKBENCH_SERVICE_FIXTURE_HPP
#pragma once
#include "Fixture.hpp"

namespace service_fixture {
using namespace workbench_fixture;
using Empty = runtime_fixture::Empty;

struct Boolean {
	bool value;
};

struct Big {
	std::array<std::uint8_t, 700> bytes;
};

struct Device {
	Packet packet = runtime_fixture::initial, last{};
	unsigned reads = 0, writes = 0, calls = 0, commands = 0;
	bool refuse = false;

	Packet read() noexcept
	{
		++reads;
		return packet;
	}

	telemetry::WriteResult write(const Packet& value) noexcept
	{
		++writes;
		packet = value;
		return telemetry::WriteResult::Applied;
	}

	telemetry::CommandResult command() noexcept
	{
		++commands;
		return telemetry::CommandResult::Executed;
	}

	telemetry::ServiceResult<Packet> transform(const Packet& value) noexcept
	{
		++calls;
		if (refuse)
			return telemetry::ServiceResult<Packet>::failure(
			    telemetry::ServiceStatus::InvalidArgument);
		last = value;
		packet = value;
		return telemetry::ServiceResult<Packet>::success(value);
	}

	telemetry::ServiceResult<Packet> current() noexcept
	{
		++calls;
		return telemetry::ServiceResult<Packet>::success(packet);
	}

	telemetry::ServiceResult<void> statusOnly() noexcept
	{
		++calls;
		return telemetry::ServiceResult<void>::success();
	}

	telemetry::ServiceResult<Empty> empty(const Empty&) noexcept
	{
		++calls;
		return telemetry::ServiceResult<Empty>::success({});
	}

	telemetry::ServiceResult<Big> big() noexcept
	{
		++calls;
		return telemetry::ServiceResult<Big>::successFrom([] {
			return Big{};
		});
	}

	telemetry::ServiceResult<Boolean> boolean() noexcept
	{
		++calls;
		return telemetry::ServiceResult<Boolean>::success({true});
	}
};

inline Device device;
inline constexpr telemetry::Model model{
    telemetry::group("device", telemetry::field<&Device::read, &Device::write>("Config", device),
                     telemetry::command<&Device::command>("Reset", device),
                     telemetry::service<&Device::transform>("Transform", device),
                     telemetry::service<&Device::current>("Current", device),
                     telemetry::service<&Device::statusOnly>("StatusOnly", device),
                     telemetry::service<&Device::empty>("Empty", device),
                     telemetry::service<&Device::big>("Big", device),
                     telemetry::service<&Device::boolean>("Boolean", device))};
inline const auto modelView = model.view();
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = telemetry::descriptor::packDescriptor<descriptor>();

struct Data {
	static auto descriptor() noexcept
	{
		return cl::Input{descriptorBytes};
	}

	static const auto& model() noexcept
	{
		return modelView;
	}

	static void reset() noexcept
	{
		device = {};
	}
};

template<std::size_t N = 4, std::size_t ResponseBytes = 1024>
using Encoded = workbench_fixture::Encoded<N, 512, 512, Data, ResponseBytes>;
using Rpc = workbench_fixture::RpcModel<Data, 1024>;

template<class Fixture>
auto select(Fixture& f, std::uint32_t id = 0) noexcept
{
	const auto bound = f.inspector.bindService(id);
	CHECK(bound);
	CHECK(f.inspector.selectService(bound.endpoint) == cl::Status::Ok);
	CHECK(f.inspector.serviceSelected() && !f.inspector.commandSelected());
	return bound.endpoint;
}

template<class Model>
auto root(Model& m) noexcept
{
	const auto n = m.editorRoot();
	CHECK(n);
	return n.value;
}

template<class Model>
auto child(Model& m, const im::NodeHandle& p, std::uint32_t i) noexcept
{
	const auto n = m.editorChild(p, i);
	CHECK(n);
	return n.value;
}

template<class Model>
auto responseRoot(Model& m) noexcept
{
	const auto n = m.serviceRoot();
	CHECK(n);
	return n.value;
}

template<class Model>
auto responseChild(Model& m, const im::NodeHandle& p, std::uint32_t i) noexcept
{
	const auto n = m.serviceChild(p, i);
	CHECK(n);
	return n.value;
}

template<class T, class Model>
T scalar(Model& m, const im::NodeHandle& node, bool response = false) noexcept
{
	const auto value = response ? m.serviceScalar(node) : m.editScalar(node);
	CHECK(value);
	const auto* p = std::get_if<T>(&value.value.value);
	CHECK(p);
	return *p;
}
} // namespace service_fixture
#endif

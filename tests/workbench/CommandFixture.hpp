/* Rich real Command models reusing the Field transport fixtures. MIT. */
#ifndef TELEMETRY_TEST_WORKBENCH_COMMAND_FIXTURE_HPP
#define TELEMETRY_TEST_WORKBENCH_COMMAND_FIXTURE_HPP
#pragma once
#include "Fixture.hpp"

namespace command_fixture {
using namespace workbench_fixture;
using Empty = runtime_fixture::Empty;

struct Big {
	std::array<std::uint8_t, 700> bytes;
};

struct Device {
	Packet packet = runtime_fixture::initial, last{};
	unsigned reads = 0, commands = 0, executed = 0, accepted = 0;
	bool refuse = false;

	Packet read() noexcept
	{
		++reads;
		return packet;
	}

	telemetry::WriteResult write(const Packet& value) noexcept
	{
		packet = value;
		return telemetry::WriteResult::Applied;
	}

	telemetry::CommandResult configure(const Packet& value) noexcept
	{
		++commands;
		if (refuse)
			return telemetry::CommandResult::InvalidValue;
		++executed;
		last = value;
		packet = value;
		return telemetry::CommandResult::Executed;
	}

	telemetry::CommandResult queue(const Packet& value) noexcept
	{
		++commands;
		++accepted;
		last = value;
		return telemetry::CommandResult::Accepted;
	}

	telemetry::CommandResult reset() noexcept
	{
		++commands;
		++executed;
		return telemetry::CommandResult::Executed;
	}

	telemetry::CommandResult empty(const Empty&) noexcept
	{
		++commands;
		++executed;
		return telemetry::CommandResult::Executed;
	}

	telemetry::CommandResult big(const Big&) noexcept
	{
		++commands;
		return telemetry::CommandResult::Executed;
	}
};

inline Device device;
inline constexpr telemetry::Model model{
    telemetry::group("device", telemetry::field<&Device::read, &Device::write>("Config", device),
                     telemetry::command<&Device::configure>("Configure", device),
                     telemetry::command<&Device::queue>("Accept", device),
                     telemetry::command<&Device::reset>("Reset", device),
                     telemetry::command<&Device::empty>("EmptyArgs", device),
                     telemetry::command<&Device::big>("BigArgs", device))};
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

template<std::size_t N = 4, std::size_t PayloadBytes = 512, std::size_t SnapshotBytes = 512>
using Encoded = workbench_fixture::Encoded<N, PayloadBytes, SnapshotBytes, Data>;
using Rpc = workbench_fixture::RpcModel<Data>;

template<class Fixture>
auto select(Fixture& fixture, std::uint32_t id = 0) noexcept
{
	const auto binding = fixture.inspector.bindCommand(id);
	CHECK(binding);
	CHECK(fixture.inspector.selectCommand(binding.endpoint) == cl::Status::Ok);
	CHECK(fixture.inspector.commandSelected() && fixture.inspector.canExecute());
	return binding.endpoint;
}

template<class Model>
auto child(Model& model, const im::NodeHandle& parent, std::uint32_t index) noexcept
{
	const auto node = model.editorChild(parent, index);
	CHECK(node);
	return node.value;
}

template<class Model>
auto root(Model& model) noexcept
{
	const auto node = model.editorRoot();
	CHECK(node);
	return node.value;
}

template<class T, class Model>
T scalar(Model& model, const im::NodeHandle& node) noexcept
{
	const auto value = model.editScalar(node);
	CHECK(value);
	const auto* number = std::get_if<T>(&value.value.value);
	CHECK(number);
	return *number;
}
} // namespace command_fixture
#endif

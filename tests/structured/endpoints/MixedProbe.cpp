/*
 * @file MixedProbe.cpp
 * @brief Mixed native/encoded parity, preflight, statuses and shared TypeIds.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "MixedFixture.hpp"
#include <type_traits>

namespace ts = telemetry;
using namespace fixture;

static_assert(std::is_same_v<decltype(mixedFields.read<0>()), std::optional<bool>>);
static_assert(std::is_same_v<decltype(mixedFields.read<1>()), std::optional<std::uint16_t>>);
static_assert(std::is_same_v<decltype(mixedFields.read<2>()), std::optional<float>>);
static_assert(std::is_same_v<decltype(mixedFields.read<3>()), std::optional<double>>);
static_assert(std::is_same_v<decltype(mixedFields.read<4>()), std::optional<Mode>>);
static_assert(
    std::is_same_v<decltype(mixedFields.read<5>()), std::optional<std::array<std::uint16_t, 3>>>);
static_assert(std::is_same_v<decltype(mixedFields.read<6>()), std::optional<State>>);
static_assert(std::is_same_v<decltype(mixedFields.read<7>()), std::optional<MotorConfig>>);
// Paths give the same value types as the local table's positions.
static_assert(
    std::is_same_v<decltype(ts::read<model, "motor/Config">()), std::optional<MotorConfig>>);
static_assert(std::is_same_v<decltype(ts::read<model, "motor/Samples">()),
                             std::optional<std::array<std::uint16_t, 3>>>);
static_assert(std::is_same_v<decltype(config)::Value, MotorConfig> && config.id == 7);
static_assert(model.view().fieldTypeId(7).value() == model.typeId<MotorConfig>());
static_assert(model.view().commandTypeId(0).value() == model.typeId<MotorConfig>());
static_assert(model.view().serviceTypeIds(0)->requestTypeId == model.typeId<MotorConfig>());
static_assert(model.view().serviceTypeIds(0)->responseTypeId == model.typeId<MotorConfig>());
static_assert(!std::is_copy_constructible_v<decltype(mixedFields)>);
static_assert(!std::is_move_constructible_v<decltype(localCommands)>);
static_assert(!std::is_copy_constructible_v<decltype(model)>);

// The local table's position I, the tree's path P and the encoded ID agree.
template<std::size_t I, ts::EndpointPath P>
bool readParity(ts::Workspace& workspace)
{
	const auto native = mixedFields.read<I>();
	const auto byPath = ts::read<model, P>();
	using Value = typename decltype(native)::value_type;
	std::array<std::byte, ts::wireSize<Value>> expected{}, pathBytes{}, encoded{};
	if (!native || !byPath || ts::encode(*native, expected) != ts::CodecStatus::Ok ||
	    ts::encode(*byPath, pathBytes) != ts::CodecStatus::Ok)
		return false;
	constexpr auto id = ts::idOf<model, P>();
	const auto result = ts::readFieldEncoded(model.view(), id, encoded, workspace);
	return id == I && result.dispatch == ts::DispatchStatus::Ok &&
	       result.written == encoded.size() && encoded == expected && pathBytes == expected &&
	       workspace.used() == 0;
}

int main()
{
	std::array<std::byte, model.maxScratch()> storage{};
	ts::Workspace workspace{storage};
	if (!(readParity<0, "motor/Enabled">(workspace) && readParity<1, "motor/RPM">(workspace) &&
	      readParity<2, "motor/Temperature">(workspace) &&
	      readParity<3, "motor/Precise">(workspace) && readParity<4, "motor/Mode">(workspace) &&
	      readParity<5, "motor/Samples">(workspace) && readParity<6, "motor/State">(workspace) &&
	      readParity<7, "motor/Config">(workspace)))
		return 1;

	MotorConfig value{250.0f, 1600, false};
	if (mixedFields.write<Position::Config>(value) != telemetry::WriteResult::Applied ||
	    config.write(value) != telemetry::WriteResult::Applied ||
	    ts::write<model, "motor/Config">(value) != telemetry::WriteResult::Applied)
		return 2;
	constexpr std::array<std::byte, 7> golden{std::byte{0},    std::byte{0},    std::byte{0x7a},
	                                          std::byte{0x43}, std::byte{0x40}, std::byte{0x06},
	                                          std::byte{0}};
	auto request = golden;
	const auto written =
	    ts::writeFieldEncoded(model.view(), telemetry::PackedId{7}, request, workspace);
	if (written.dispatch != ts::DispatchStatus::Ok ||
	    written.endpointStatus != telemetry::WriteResult::Applied || device.current.rpm != 1600 ||
	    device.current.target != 250.0f || device.current.enabled)
		return 3;

	if (localCommands.call<0>(value) != telemetry::CommandResult::Executed ||
	    configure.call(value) != telemetry::CommandResult::Executed ||
	    ts::call<model, "motor/Configure">(value) != telemetry::CommandResult::Executed)
		return 4;
	const auto executed =
	    ts::executeCommandEncoded(model.view(), telemetry::PackedId{0}, request, workspace);
	if (executed.dispatch != ts::DispatchStatus::Ok ||
	    executed.endpointStatus != telemetry::CommandResult::Executed)
		return 5;
	std::array<std::byte, 7> output{};
	const auto service =
	    ts::callServiceEncoded(model.view(), telemetry::PackedId{0}, request, output, workspace);
	if (service.dispatch != ts::DispatchStatus::Ok || output != golden)
		return 6;

	const int readsBefore = device.reads, writesBefore = device.writes,
	          commandsBefore = device.commands;
	output.fill(std::byte{0x55});
	const auto beforeOutput = output;
	if (model.fieldIndex().readEncoded(7, std::span{output}.first(6), workspace).dispatch !=
	        ts::DispatchStatus::BufferTooSmall ||
	    device.reads != readsBefore || output != beforeOutput)
		return 7;
	std::array<std::byte, 1> tiny{};
	ts::Workspace small{tiny};
	if constexpr (sizeof(MotorConfig) > ts::maxLocalObjectBytes) {
		if (model.fieldIndex().readEncoded(7, output, small).dispatch !=
		        ts::DispatchStatus::WorkspaceTooSmall ||
		    model.fieldIndex().writeEncoded(7, request, small).dispatch !=
		        ts::DispatchStatus::WorkspaceTooSmall ||
		    model.commandIndex().executeEncoded(0, request, small).dispatch !=
		        ts::DispatchStatus::WorkspaceTooSmall)
			return 8;
	}
	request.back() = std::byte{2};
	if (model.fieldIndex().writeEncoded(7, request, workspace).dispatch !=
	        ts::DispatchStatus::InvalidPayload ||
	    model.commandIndex().executeEncoded(0, request, workspace).dispatch !=
	        ts::DispatchStatus::InvalidPayload ||
	    device.reads != readsBefore || device.writes != writesBefore ||
	    device.commands != commandsBefore || workspace.used() != 0 || output != beforeOutput)
		return 9;
	request = golden;
	if (model.fieldIndex().writeEncoded(0, {}, small).endpointStatus !=
	        telemetry::WriteResult::ReadOnly ||
	    mixedFields.write<0>(true) != telemetry::WriteResult::ReadOnly)
		return 10;

	for (unsigned status = 0; status < 8; ++status) {
		device.commandStatus = static_cast<telemetry::CommandResult>(status);
		if (localCommands.call<1>() != device.commandStatus)
			return 11;
		const auto result = model.commandIndex().executeEncoded(1, {}, workspace);
		if (result.dispatch != ts::DispatchStatus::Ok ||
		    result.endpointStatus != device.commandStatus)
			return 12;
	}
	for (unsigned status = 0; status < 6; ++status) {
		device.writeStatus = static_cast<telemetry::WriteResult>(status);
		const auto result = model.fieldIndex().writeEncoded(7, request, workspace);
		if (result.dispatch != ts::DispatchStatus::Ok ||
		    result.endpointStatus != device.writeStatus)
			return 13;
	}
	device.commandStatus = static_cast<telemetry::CommandResult>(255);
	device.writeStatus = static_cast<telemetry::WriteResult>(255);
	if (model.commandIndex().executeEncoded(1, {}, workspace).dispatch !=
	        ts::DispatchStatus::InternalError ||
	    model.fieldIndex().writeEncoded(7, request, workspace).dispatch !=
	        ts::DispatchStatus::InternalError)
		return 14;
	if (model.fieldIndex().find(std::uint64_t{0x100000000ULL}) || model.commandIndex().find(-1) ||
	    model.view().fieldTypeId(ts::detail::makeId(1, 0)) ||
	    model.view().commandTypeId(ts::detail::makeId(0, 2)))
		return 15;
	return 0;
}

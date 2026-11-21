/*
 * @file Edges.cpp
 * @brief Empty groups, u32 routing, buffer aliasing and exact input sizes.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "MixedFixture.hpp"
namespace ts = telemetry;
using namespace fixture;

// A tree of one empty group holds no endpoint of any family.
inline constexpr ts::Model emptyModel{ts::group("empty")};
static_assert(emptyModel.maxScratch() == 0);
static_assert(!emptyModel.fieldIndex().find(0) && !emptyModel.commandIndex().find(0));
// An empty group first moves meter's endpoints to group 1 in every family.
inline constexpr ts::Model grouped{
    ts::group("empty"),
    ts::group("meter", enabledField, rpmField, temperatureField, preciseField, modeField,
              samplesField, stateField, configField, configureCommand, resetCommand, echoService)};
static_assert(ts::idOf<grouped, "meter/Config">() == 0x00010007u &&
              ts::idOf<grouped, "meter/Configure">() == 0x00010000u);
static_assert(grouped.view().fieldTypeId(ts::idOf<grouped, "meter/Config">()) ==
              model.typeId<MotorConfig>());
static_assert(grouped.view().commandTypeId(ts::idOf<grouped, "meter/Configure">()) ==
              model.typeId<MotorConfig>());

int main()
{
	std::array<std::byte, 128> storage{};
	std::array<std::byte, 8> input{}, output{};
	ts::Workspace workspace{storage};
	const auto f = grouped.fieldIndex();
	const auto c = grouped.commandIndex();
	constexpr auto fieldId = ts::idOf<grouped, "meter/Config">();
	constexpr auto commandId = ts::idOf<grouped, "meter/Configure">();
	const auto unchanged = [] {
		return device.reads + device.writes + device.commands == 0;
	};
	for (std::uint64_t id :
	     std::array<std::uint64_t, 5>{0, 65535, 0x20000, 0x100010007ULL, UINT64_MAX}) {
		if (f.readEncoded(id, output, workspace).dispatch != ts::DispatchStatus::NotFound ||
		    f.writeEncoded(id, input, workspace).dispatch != ts::DispatchStatus::NotFound ||
		    c.executeEncoded(id, input, workspace).dispatch != ts::DispatchStatus::NotFound)
			return 1;
	}
	if (f.find(-1) || c.find(-1) || f.find(0x10008u) || c.find(0x10002u))
		return 2;
	for (std::size_t count : {std::size_t{0}, std::size_t{6}, std::size_t{8}}) {
		const auto bytes = std::span{input}.first(count);
		if (f.writeEncoded(fieldId, bytes, workspace).dispatch !=
		        ts::DispatchStatus::InvalidPayload ||
		    c.executeEncoded(commandId, bytes, workspace).dispatch !=
		        ts::DispatchStatus::InvalidPayload)
			return 3;
	}
	if (!unchanged())
		return 4;
	const auto overlap = std::span{storage}.first(7);
	constexpr auto overlapStatus = sizeof(MotorConfig) <= ts::maxLocalObjectBytes
	                                   ? ts::DispatchStatus::Ok
	                                   : ts::DispatchStatus::InvalidPayload;
	if (f.readEncoded(fieldId, overlap, workspace).dispatch != overlapStatus ||
	    f.writeEncoded(fieldId, overlap, workspace).dispatch != overlapStatus ||
	    c.executeEncoded(commandId, overlap, workspace).dispatch != overlapStatus ||
	    (overlapStatus != ts::DispatchStatus::Ok && !unchanged()) || workspace.used() != 0)
		return 4;

	// Four remaining bytes cannot hold MotorConfig in Workspace. A local
	// policy can still execute, while keeping the outer lease unchanged.
	{
		auto outer = workspace.reserve<std::array<std::byte, 124>>();
		if (!outer.valid() || outer.constructFrom([] {
			    return std::array<std::byte, 124>{};
		    }) == nullptr)
			return 5;
		const auto used = workspace.used();
		constexpr auto expected = sizeof(MotorConfig) <= ts::maxLocalObjectBytes
		                              ? ts::DispatchStatus::Ok
		                              : ts::DispatchStatus::WorkspaceTooSmall;
		if (f.readEncoded(fieldId, output, workspace).dispatch != expected ||
		    f.writeEncoded(fieldId, std::span{input}.first(7), workspace).dispatch != expected ||
		    c.executeEncoded(commandId, std::span{input}.first(7), workspace).dispatch !=
		        expected ||
		    workspace.used() != used || (expected != ts::DispatchStatus::Ok && !unchanged()))
			return 6;
	}
	if (workspace.used() != 0)
		return 7;
	MotorConfig next{15.f, 19, true};
	if (ts::encode(next, std::span{input}.first(7)) != ts::CodecStatus::Ok)
		return 8;
	if (f.writeEncoded(fieldId, std::span{input}.first(7), workspace).endpointStatus !=
	        telemetry::WriteResult::Applied ||
	    c.executeEncoded(commandId, std::span{input}.first(7), workspace).endpointStatus !=
	        telemetry::CommandResult::Executed ||
	    ts::read<grouped, "meter/Config.rpm">() != std::uint16_t{19} || workspace.used() != 0)
		return 9;
	ts::Workspace noStorage{std::span<std::byte>{}};
	if (c.executeEncoded(ts::idOf<grouped, "meter/Reset">(), {}, noStorage).dispatch !=
	    ts::DispatchStatus::Ok)
		return 10;
	return 0;
}

/*
 * @file ModelProbe.cpp
 * @brief Three Service call paths and encoded preflight checks.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ts = telemetry;

namespace probe {

struct ReadCalibrationRequest {
	std::uint16_t channel;
	bool adjusted;
};

struct ReadCalibrationResponse {
	std::uint32_t scale;
	bool adjusted;
};

inline int calls = 0;

// Calibration Service owner exposes counted native and encoded calls to the same DTOs.
// API: readCalibration().
struct Device {
	std::uint32_t base = 100;

	ReadCalibrationResponse readCalibration(const ReadCalibrationRequest& request) const noexcept
	{
		++calls;
		return {base + request.channel, request.adjusted};
	}
};

inline Device device{};
inline telemetry::OwnerSlot<Device> ownerSlot;

inline constexpr ts::ServiceTable local{
    ts::service<&Device::readCalibration>("ReadCalibration", device),
    ts::service<&Device::readCalibration>("LateRead", ownerSlot)};

// The device: one group of two Services, the second behind an owner slot.
inline constexpr ts::Model model{
    ts::group("calibration", ts::service<&Device::readCalibration>("ReadCalibration", device),
              ts::service<&Device::readCalibration>("LateRead", ownerSlot))};
inline constexpr auto readCalibration = ts::endpoint<model, "calibration/ReadCalibration">;

// The catalog engine under the tree, through the testing header: decltype on
// the inline constexpr empty markers retains const, and explicit Model
// arguments must support those same live objects as deduction does.
namespace v30 = ts::testing;
inline constexpr v30::ServiceCatalogTable services{v30::group("calibration", local)};
using ConstEmptyModel =
    v30::V30Model<decltype(v30::emptyFields), decltype(v30::emptyCommands), decltype(services)>;
inline constexpr ConstEmptyModel constEmptyModel{v30::emptyFields, v30::emptyCommands, services};
inline constexpr auto constEmptyView = constEmptyModel.view();
static_assert(constEmptyModel.fieldIndex().count() == 0);
static_assert(constEmptyModel.commandIndex().count() == 0);
static_assert(constEmptyView.fields.count() == 0 && constEmptyView.fieldTypes == nullptr &&
              constEmptyView.fieldCatalogCount == 0);
static_assert(constEmptyView.commands.count() == 0 && constEmptyView.commandTypes == nullptr &&
              constEmptyView.commandCatalogCount == 0);
static_assert(constEmptyView.serviceCatalogCount == services.size());
static_assert(constEmptyView.serviceTypeIds(readCalibration.id)->requestTypeId ==
              model.typeId<ReadCalibrationRequest>());
static_assert(constEmptyModel.maxScratch() == model.maxScratch());

std::uint32_t readValue() noexcept
{
	return 7;
}

ts::CommandResult execute() noexcept
{
	return ts::CommandResult::Executed;
}

inline constexpr ts::FieldTable constLocalFields{ts::field<&readValue>("Value")};
inline constexpr ts::CommandTable constLocalCommands{ts::command<&execute>("Execute")};
inline constexpr v30::FieldCatalogTable constFields{v30::group("fields", constLocalFields)};
inline constexpr v30::CommandCatalogTable constCommands{v30::group("commands", constLocalCommands)};
using ConstFullModel =
    v30::V30Model<decltype(constFields), decltype(constCommands), decltype(services)>;
inline constexpr ConstFullModel constFullModel{constFields, constCommands, services};
inline constexpr auto constFullView = constFullModel.view();
static_assert(constFullView.fields.count() == 1 && constFullView.fieldCatalogCount == 1);
static_assert(constFullView.commands.count() == 1 && constFullView.commandCatalogCount == 1);
static_assert(constFullView.serviceCatalogCount == services.size());
static_assert(constFullView.fieldTypeId(0u) == constFullModel.typeId<std::uint32_t>());
static_assert(constFullView.commandTypeId(0u) == constFullModel.typeId<void>());
static_assert(constFullView.serviceTypeIds(0u)->responseTypeId ==
              constFullModel.typeId<ReadCalibrationResponse>());
static_assert(constFullModel.maxFieldWireSize() == sizeof(std::uint32_t));

static_assert(local.size() == 2);
static_assert(services.size() == 1);
static_assert(model.types().count >= 14);
static_assert(model.template typeId<ReadCalibrationRequest>() !=
              model.template typeId<ReadCalibrationResponse>());
static_assert(model.maxServiceResponseWireSize() == 5);
static_assert(!std::is_copy_constructible_v<decltype(local)>);
static_assert(!std::is_move_constructible_v<decltype(services)>);
static_assert(!std::is_copy_constructible_v<decltype(model)>);
// A tree has a row per group in every family; this one holds Services only.
static_assert(model.view().fieldCatalogCount == 1 && model.fieldIndex().find(0u) == nullptr);

} // namespace probe

int main()
{
	using Request = probe::ReadCalibrationRequest;
	using Response = probe::ReadCalibrationResponse;
	constexpr auto id = probe::readCalibration.id;
	constexpr auto slotId = ts::idOf<probe::model, "calibration/LateRead">();
	const Request request{7, true};

	auto local = probe::local.call<0>(request);
	auto byHandle = probe::readCalibration.call(request);
	auto byPath = ts::call<probe::model, "calibration/ReadCalibration">(request);
	if (local.value().scale != 107 || byHandle.value().scale != 107 ||
	    byPath.value().scale != 107 || probe::calls != 3)
		return 1;
	probe::calls = 2;

	std::array<std::byte, ts::wireSize<Request>> input{};
	std::array<std::byte, ts::wireSize<Response>> output{};
	std::array<std::byte, 128> scratch{};
	ts::Workspace workspace{scratch};
	if (ts::encode(request, input) != ts::CodecStatus::Ok)
		return 2;

	const auto index = probe::model.serviceIndex();
	auto encoded = index.callEncoded(id, input, output, workspace);
	if (encoded.dispatch != ts::DispatchStatus::Ok ||
	    encoded.endpointStatus != ts::ServiceStatus::Ok || encoded.written != output.size() ||
	    probe::calls != 3 || workspace.used() != 0)
		return 3;
	{
		auto responseLease = workspace.reserve<Response>();
		Response* response = nullptr;
		if (ts::decode(output, responseLease, response) != ts::CodecStatus::Ok ||
		    response->scale != 107 || !response->adjusted)
			return 4;
	}

	const int before = probe::calls;
	const auto priorOutput = output;
	if (index.callEncoded(id, input, std::span{output}.first(4), workspace).dispatch !=
	        ts::DispatchStatus::BufferTooSmall ||
	    probe::calls != before || output != priorOutput || workspace.used() != 0)
		return 5;
	std::array<std::byte, 1> tiny{};
	ts::Workspace small{tiny};
	if constexpr (probe::model.maxServiceScratch() > 1) {
		if (index.callEncoded(id, input, output, small).dispatch !=
		        ts::DispatchStatus::WorkspaceTooSmall ||
		    probe::calls != before || output != priorOutput)
			return 6;
	}

	input.back() = std::byte{2};
	if (index.callEncoded(id, input, output, workspace).dispatch !=
	        ts::DispatchStatus::InvalidPayload ||
	    probe::calls != before || output != priorOutput || workspace.used() != 0)
		return 7;
	if (index.callEncoded(slotId, input, output, workspace).dispatch !=
	        ts::DispatchStatus::InvalidPayload ||
	    probe::calls != before)
		return 14;
	input.back() = std::byte{1};

	if (index.callEncoded(slotId, input, output, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    probe::calls != before)
		return 8;
	probe::ownerSlot.bind(probe::device);
	if (index.callEncoded(slotId, input, output, workspace).dispatch != ts::DispatchStatus::Ok ||
	    probe::calls != before + 1)
		return 9;
	probe::ownerSlot.reset();

	if (index.find(std::uint64_t{0x100000000ULL}) != nullptr ||
	    index.callEncoded(std::uint64_t{0x100000000ULL}, input, output, workspace).dispatch !=
	        ts::DispatchStatus::NotFound)
		return 10;
	if (probe::model.view().serviceTypeIds(id)->requestTypeId !=
	    probe::model.template typeId<Request>())
		return 11;
	if (probe::model.view().serviceTypeIds(id)->responseTypeId !=
	    probe::model.template typeId<Response>())
		return 12;
	auto viaAdapter = ts::callServiceEncoded(probe::model.view(), id, input, output, workspace);
	if (viaAdapter.dispatch != ts::DispatchStatus::Ok || viaAdapter.written != output.size() ||
	    probe::calls != before + 2)
		return 13;
	return 0;
}

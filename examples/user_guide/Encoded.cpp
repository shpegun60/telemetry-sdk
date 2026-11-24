/*
 * @file Encoded.cpp
 * @brief Application packet routing to Model operations, without resource files.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 *
 * Show how an application packet selects existing encoded Model operations.
 * The adapter owns its small header and serializes the shared Workspace;
 * telemetry handles type bytes while Device decides semantic acceptance.
 */

#include <telemetry/Telemetry.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace guide {
namespace ts = telemetry;
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;

struct Config {
	std::uint32_t limit;
	bool enabled;
};

struct Increment {
	std::uint32_t amount;
};

struct Query {
	bool allow;
};

struct Report {
	std::array<std::uint32_t, 16> values;
};

// Public methods:
// - readConfig(): Borrow current configuration.
// - readReport(): Borrow large report.
// - writeConfig(): Apply validated configuration.
// - increment(): Apply bounded increment.
// - reset(): Clear current counter.
// - copyReport(): Return owning report.
// - viewReport(): Return borrowed report.
struct Device {
	Config config{400, true};
	Report report{{11, 22}};
	std::uint32_t counter = 0;
	bool busy = false;
	unsigned reads = 0, writes = 0, commands = 0, copies = 0, views = 0;

	const Config& readConfig() noexcept
	{
		++reads;
		return config;
	}

	const Report& readReport() const noexcept
	{
		return report;
	}

	ts::WriteResult writeConfig(const Config& next) noexcept
	{
		++writes;
		if (next.limit == 0 || next.limit > 10000)
			return ts::WriteResult::InvalidValue;
		config = next;
		return ts::WriteResult::Applied;
	}

	ts::CommandResult increment(const Increment& request) noexcept
	{
		++commands;
		if (request.amount > 100)
			return ts::CommandResult::InvalidValue;
		counter += request.amount;
		return ts::CommandResult::Executed;
	}

	ts::CommandResult reset() noexcept
	{
		++commands;
		counter = 0;
		return ts::CommandResult::Executed;
	}

	ts::ServiceResult<Report> copyReport(const Query& request) noexcept
	{
		++copies;
		if (!request.allow)
			return ts::ServiceResult<Report>::failure(ts::ServiceStatus::InvalidArgument);
		if (busy)
			return ts::ServiceResult<Report>::failure(ts::ServiceStatus::Busy);
		return ts::ServiceResult<Report>::successFrom([this]() noexcept {
			return report;
		});
	}

	ts::BorrowedServiceResult<Report> viewReport(const Query& request) noexcept
	{
		++views;
		if (!request.allow)
			return ts::BorrowedServiceResult<Report>::failure(ts::ServiceStatus::InvalidArgument);
		if (busy)
			return ts::BorrowedServiceResult<Report>::failure(ts::ServiceStatus::Busy);
		return ts::BorrowedServiceResult<Report>::success(report);
	}
};

inline Device device;
inline constexpr ts::Model model{
    ts::group("device", ts::field<&Device::readConfig, &Device::writeConfig>("Config", device),
              ts::field<&Device::readReport>("Report", device),
              ts::command<&Device::increment>("Increment", device),
              ts::command<&Device::reset>("Reset", device),
              ts::service<&Device::copyReport>("CopyReport", device),
              ts::service<&Device::viewReport>("ViewReport", device))};
inline constexpr auto view = model.view();
// The runtime IDs a packet carries, each from its endpoint's path. The three
// families share the group's position and number their own entries.
inline constexpr ts::PackedId ConfigId = ts::idOf<model, "device/Config">();
inline constexpr ts::PackedId ReportId = ts::idOf<model, "device/Report">();
inline constexpr ts::PackedId IncrementId = ts::idOf<model, "device/Increment">();
inline constexpr ts::PackedId ResetId = ts::idOf<model, "device/Reset">();
inline constexpr ts::PackedId CopyReportId = ts::idOf<model, "device/CopyReport">();
inline constexpr ts::PackedId ViewReportId = ts::idOf<model, "device/ViewReport">();
static_assert(ConfigId == 0 && ReportId == 1 && IncrementId == 0 && ViewReportId == 1);
static_assert(ts::wireSize<Config> == 5 && ts::wireSize<Report> == 64);
static_assert(ts::maxLocalObjectBytes == 32,
              "This example demonstrates the default local payload budget");
static_assert(model.maxScratch() > 0); // The owning 64-byte Service result needs scratch.
inline std::array<std::byte, model.maxScratch()> scratch{};
inline ts::Workspace workspace{scratch};

// An application-specific header for this example: u8 operation, u32 LE ID,
// then the canonical payload. These codes/header are not a telemetry protocol.
enum class Operation : std::uint8_t {
	Read = 1,
	Write = 2,
	Command = 3,
	Service = 4
};

struct Reply {
	ts::DispatchStatus dispatch;
	std::uint8_t endpointStatus;
	std::uint32_t written;
}; // In-process result. A real transport encodes its own reply envelope.

std::uint32_t get32(Input bytes, std::size_t offset) noexcept
{
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t{std::to_integer<unsigned char>(bytes[offset + i])} << (8 * i);
	return value;
}

void header(Output packet, Operation operation, ts::PackedId id) noexcept
{
	packet[0] = static_cast<std::byte>(operation);
	for (unsigned i = 0; i < 4; ++i)
		packet[1 + i] = static_cast<std::byte>(id >> (8 * i));
}

std::array<std::byte, 5> emptyPacket(Operation operation, ts::PackedId id) noexcept
{
	std::array<std::byte, 5> packet{};
	header(packet, operation, id);
	return packet;
}

template<class T>
auto packet(Operation operation, ts::PackedId id, const T& value) noexcept
{
	std::array<std::byte, 5 + ts::wireSize<T>> bytes{};
	header(bytes, operation, id);
	if (ts::encode(value, Output{bytes}.subspan(5)) != ts::CodecStatus::Ok)
		std::abort();
	return bytes;
}

// Called only after application framing has produced one whole request.
// Repeated packets execute again; no retry or duplicate policy is inferred.
Reply onCompletePacket(Input complete, Output response, ts::Workspace& operationWorkspace) noexcept
{
	if (complete.size() < 5)
		return {ts::DispatchStatus::InvalidPayload, 0, 0};
	const auto operation = static_cast<Operation>(std::to_integer<std::uint8_t>(complete[0]));
	const ts::PackedId id = get32(complete, 1);
	const auto payload = complete.subspan(5);
	switch (operation) {
		case Operation::Read: {
			if (!payload.empty())
				return {ts::DispatchStatus::InvalidPayload, 0, 0};
			const auto result = ts::readFieldEncoded(view, id, response, operationWorkspace);
			return {result.dispatch, 0, result.written};
		}
		case Operation::Write: {
			const auto result = ts::writeFieldEncoded(view, id, payload, operationWorkspace);
			return {result.dispatch,
			        result.dispatch == ts::DispatchStatus::Ok
			            ? static_cast<std::uint8_t>(result.endpointStatus)
			            : std::uint8_t{0},
			        0};
		}
		case Operation::Command: {
			const auto result = ts::executeCommandEncoded(view, id, payload, operationWorkspace);
			return {result.dispatch,
			        result.dispatch == ts::DispatchStatus::Ok
			            ? static_cast<std::uint8_t>(result.endpointStatus)
			            : std::uint8_t{0},
			        0};
		}
		case Operation::Service: {
			const auto result =
			    ts::callServiceEncoded(view, id, payload, response, operationWorkspace);
			return {result.dispatch,
			        result.dispatch == ts::DispatchStatus::Ok
			            ? static_cast<std::uint8_t>(result.endpointStatus)
			            : std::uint8_t{0},
			        result.written};
		}
	}
	return {ts::DispatchStatus::InvalidPayload, 0, 0};
}
} // namespace guide

int main()
{
	using namespace guide;
	unsigned checks = 0;
	const auto check = [&checks](bool ok) {
		++checks;
		if (!ok)
			std::abort();
	};
	std::array<std::byte, ts::wireSize<Report>> output{};
	ts::Workspace noScratch{Output{}};

	const auto read = emptyPacket(Operation::Read, ConfigId);
	auto reply = onCompletePacket(read, output, workspace);
	check(reply.dispatch == ts::DispatchStatus::Ok && reply.written == 5 &&
	      get32(output, 0) == 400);
	check(output[4] == std::byte{1} && device.reads == 1);
	reply = onCompletePacket(read, Output{output}.first(4), workspace);
	check(reply.dispatch == ts::DispatchStatus::BufferTooSmall && device.reads == 1);

	const auto write = packet(Operation::Write, ConfigId, Config{900, false});
	reply = onCompletePacket(write, output, workspace);
	check(reply.dispatch == ts::DispatchStatus::Ok &&
	      reply.endpointStatus == static_cast<unsigned>(ts::WriteResult::Applied));
	check(device.config.limit == 900 && !device.config.enabled && device.writes == 1);

	auto badBool = write;
	badBool.back() = std::byte{2};
	reply = onCompletePacket(badBool, output, workspace);
	check(reply.dispatch == ts::DispatchStatus::InvalidPayload && device.writes == 1);
	const auto invalidValue = packet(Operation::Write, ConfigId, Config{0, false});
	reply = onCompletePacket(invalidValue, output, workspace);
	check(reply.dispatch == ts::DispatchStatus::Ok &&
	      reply.endpointStatus == static_cast<unsigned>(ts::WriteResult::InvalidValue));
	check(device.config.limit == 900 && device.writes == 2);

	reply = onCompletePacket(emptyPacket(Operation::Write, ReportId), output, workspace);
	check(reply.dispatch == ts::DispatchStatus::Ok &&
	      reply.endpointStatus == static_cast<unsigned>(ts::WriteResult::ReadOnly));
	reply = onCompletePacket(emptyPacket(Operation::Read, UINT32_MAX), output, workspace);
	check(reply.dispatch == ts::DispatchStatus::NotFound);
	reply = onCompletePacket(Input{read}.first(4), output, workspace);
	check(reply.dispatch == ts::DispatchStatus::InvalidPayload);

	const auto increment = packet(Operation::Command, IncrementId, Increment{7});
	reply = onCompletePacket(increment, output, workspace);
	check(reply.dispatch == ts::DispatchStatus::Ok &&
	      reply.endpointStatus == static_cast<unsigned>(ts::CommandResult::Executed));
	check(device.counter == 7);
	reply = onCompletePacket(increment, output, workspace); // Same packet executes again.
	check(reply.dispatch == ts::DispatchStatus::Ok && device.counter == 14 && device.commands == 2);
	reply = onCompletePacket(emptyPacket(Operation::Command, ResetId), output, workspace);
	check(reply.dispatch == ts::DispatchStatus::Ok && device.counter == 0 && device.commands == 3);

	const auto copy = packet(Operation::Service, CopyReportId, Query{true});
	reply = onCompletePacket(copy, output, workspace);
	check(reply.dispatch == ts::DispatchStatus::Ok &&
	      reply.endpointStatus == static_cast<unsigned>(ts::ServiceStatus::Ok));
	check(reply.written == 64 && get32(output, 0) == 11 && device.copies == 1);
	check(workspace.used() == 0);
	reply = onCompletePacket(copy, output, noScratch);
	check(reply.dispatch == ts::DispatchStatus::WorkspaceTooSmall && device.copies == 1);

	// Borrowed output encodes the existing Report without a response lease.
	const auto borrow = packet(Operation::Service, ViewReportId, Query{true});
	reply = onCompletePacket(borrow, output, noScratch);
	check(reply.dispatch == ts::DispatchStatus::Ok && reply.written == 64 && device.views == 1);
	check(get32(output, 0) == 11 && get32(output, 4) == 22 && noScratch.used() == 0);
	reply = onCompletePacket(emptyPacket(Operation::Read, ReportId), output, noScratch);
	check(reply.dispatch == ts::DispatchStatus::Ok && reply.written == 64);

	device.busy = true;
	reply = onCompletePacket(borrow, output, noScratch);
	check(reply.dispatch == ts::DispatchStatus::Ok &&
	      reply.endpointStatus == static_cast<unsigned>(ts::ServiceStatus::Busy));
	check(reply.written == 0 && device.views == 2);
	reply = onCompletePacket(copy, Output{output}.first(63), workspace);
	check(reply.dispatch == ts::DispatchStatus::BufferTooSmall && device.copies == 1);
	check(workspace.used() == 0);

	std::printf("Encoded guide: %u checks passed\n", checks);
}

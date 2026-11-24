/*
 * Own the group tree, runtime routing and example stream assembly.
 *
 * One translation unit keeps process-lifetime owners and shared Workspace
 * behind an ordinary application facade. Direct endpoint packets and generic
 * resource packets share framing but retain their own result envelopes.
 *
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */

#include "Api.hpp"
#include <telemetry/Telemetry.hpp>
#include <resource/Resource.hpp>
#include <resource/protocol/Protocol.hpp>
#include <algorithm>
#include <cstring>

namespace app::api {
namespace {
namespace ts = telemetry;
// Owner and every descriptor/provider have a stable address for the process.
// There is no init ordering dependency on application calls or a live device.
constinit Device device;

ts::ServiceResult<Snapshot> query(const Query& request) noexcept
{
	const auto result = device.query(request);
	if (!result)
		return ts::ServiceResult<Snapshot>::failure(ts::ServiceStatus::InvalidArgument);
	return ts::ServiceResult<Snapshot>::success(*result);
}

// One group holds the whole device: its Fields, Commands and Services.
constexpr ts::Model model{
    ts::group("device", ts::field<&Device::readPeriod, &Device::writePeriod>("Period", device),
              ts::field<&Device::readMode, &Device::writeMode>("Mode", device),
              ts::field<&Device::readGains, &Device::writeGains>("Gains", device),
              ts::field<&Device::readDisplay, &Device::writeDisplay>("Display", device),
              ts::command<&Device::reset>("Reset", device),
              ts::command<&Device::configure>("Configure", device),
              ts::service<&Device::sample>("Sample", device), ts::service<&query>("Query"))};
constexpr auto modelView = model.view();
std::array<std::byte, model.maxScratch()> scratch{};
ts::Workspace workspace{scratch};

// Named handles for the facade's native operations.
constexpr auto period = ts::endpoint<model, "device/Period">;
constexpr auto mode = ts::endpoint<model, "device/Mode">;
constexpr auto gains = ts::endpoint<model, "device/Gains">;
constexpr auto display = ts::endpoint<model, "device/Display">;

// The published IDs of Api.hpp are the IDs of these paths.
template<class Id>
constexpr std::uint32_t wire(Id id) noexcept
{
	return static_cast<std::uint32_t>(id);
}

static_assert(wire(FieldId::Period) == period.id && wire(FieldId::Mode) == mode.id);
static_assert(wire(FieldId::Gains) == gains.id && wire(FieldId::Display) == display.id);
static_assert(wire(CommandId::Reset) == ts::idOf<model, "device/Reset">());
static_assert(wire(CommandId::Configure) == ts::idOf<model, "device/Configure">());
static_assert(wire(ServiceId::Sample) == ts::idOf<model, "device/Sample">());
static_assert(wire(ServiceId::Query) == ts::idOf<model, "device/Query">());
static_assert(ts::wireSize<DisplayConfig> == 2 && ts::wireSize<Gains> == 6);
static_assert(ts::wireSize<Configure> == 13 && ts::wireSize<Snapshot> == 8);
static_assert(static_cast<unsigned>(Dispatch::Ok) == static_cast<unsigned>(ts::DispatchStatus::Ok));
static_assert(static_cast<unsigned>(Dispatch::NotFound) ==
              static_cast<unsigned>(ts::DispatchStatus::NotFound));
static_assert(static_cast<unsigned>(Dispatch::InvalidPayload) ==
              static_cast<unsigned>(ts::DispatchStatus::InvalidPayload));
static_assert(static_cast<unsigned>(Dispatch::BufferTooSmall) ==
              static_cast<unsigned>(ts::DispatchStatus::BufferTooSmall));
static_assert(static_cast<unsigned>(Dispatch::WorkspaceTooSmall) ==
              static_cast<unsigned>(ts::DispatchStatus::WorkspaceTooSmall));
static_assert(static_cast<unsigned>(Dispatch::InternalError) ==
              static_cast<unsigned>(ts::DispatchStatus::InternalError));
static_assert(static_cast<unsigned>(Dispatch::Unavailable) ==
              static_cast<unsigned>(ts::DispatchStatus::Unavailable));

// An application provider with bounded RAM and partial WRITE consumption.
// It stores an independent note, not a telemetry endpoint or durable file.
// Public methods:
// - size(): Report RAM extent.
// - read(): Read RAM slice.
// - write(): Commit bounded prefix.
class NoteFile {
public:
	resource::FileSize size() const noexcept
	{
		return bytes_.size();
	}

	resource::ReadResult read(resource::Cursor cursor, Output output) const noexcept
	{
		++reads;
		return resource::BytesFile{bytes_}.read(cursor, output);
	}

	resource::WriteResult write(resource::Cursor cursor, Input input, bool final) noexcept
	{
		if (cursor > size())
			return {resource::Status::InvalidCursor, cursor};
		if (input.size() > size() - static_cast<resource::FileSize>(cursor))
			return {resource::Status::InvalidData, cursor};
		if (input.empty())
			return final ? resource::WriteResult{resource::Status::Ok, cursor, 0, true}
			             : resource::WriteResult{resource::Status::BufferTooSmall, cursor};
		const auto consumed = static_cast<std::uint32_t>(std::min(input.size(), std::size_t{2}));
		std::memmove(bytes_.data() + static_cast<std::size_t>(cursor), input.data(), consumed);
		++writes;
		return {resource::Status::Ok, cursor + consumed, consumed,
		        final && consumed == input.size()};
	}

	mutable unsigned reads = 0;
	unsigned writes = 0;

private:
	std::array<std::byte, 16> bytes_{};
};

constexpr std::array versionBytes{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
constexpr resource::BytesFile versionFile{versionBytes};
constinit NoteFile noteFile;
constexpr auto files = resource::filesystem(resource::file("/device/version.bin", versionFile),
                                            resource::file("/device/note.bin", noteFile));

std::uint32_t get32(Input bytes, std::size_t offset) noexcept
{
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t{std::to_integer<unsigned char>(bytes[offset + i])} << (8 * i);
	return value;
}

PacketReply invalidPacket(Output response) noexcept
{
	if (response.size() < 2)
		return {PacketStatus::BufferTooSmall, 0};
	response[0] = static_cast<std::byte>(Route::Error);
	response[1] = std::byte{1};
	return {PacketStatus::InvalidPacket, 2};
}

template<class T, class Handle>
bool nativeRead(Handle handle, T& value) noexcept
{
	const auto result = handle.read();
	if (!result)
		return false;
	value = *result;
	return true;
}
} // namespace

bool readPeriod(std::uint32_t& value) noexcept
{
	return nativeRead(period, value);
}

WriteStatus writePeriod(std::uint32_t value) noexcept
{
	// Native access by handle: the path was checked where the handle is declared.
	return period.write(value);
}

bool readMode(Mode& value) noexcept
{
	return nativeRead(mode, value);
}

WriteStatus writeMode(Mode value) noexcept
{
	return mode.write(value);
}

bool readGains(Gains& value) noexcept
{
	return nativeRead(gains, value);
}

WriteStatus writeGains(const Gains& value) noexcept
{
	return gains.write(value);
}

bool readDisplay(DisplayConfig& value) noexcept
{
	return nativeRead(display, value);
}

WriteStatus writeDisplay(const DisplayConfig& value) noexcept
{
	return display.write(value);
}

Diagnostics diagnostics() noexcept
{
	return {device.counters(), noteFile.reads, noteFile.writes};
}

PacketReply onCompletePacket(Input body, Output response) noexcept
{
	if (body.empty() || body.size() > MaxBodyBytes)
		return invalidPacket(response);
	const auto route = static_cast<Route>(std::to_integer<std::uint8_t>(body[0]));
	if (route == Route::Resource) {
		if (response.size() < 2)
			return {PacketStatus::BufferTooSmall, 0};
		const auto reply =
		    resource::protocol::process(files.view(), body.subspan(1), response.subspan(1));
		if (reply.written == 0)
			return {PacketStatus::BufferTooSmall, 0};
		response[0] = static_cast<std::byte>(route);
		return {PacketStatus::Replied, 1 + reply.written};
	}
	if (route != Route::Direct || body.size() < 6)
		return invalidPacket(response);
	if (response.size() < 3)
		return {PacketStatus::BufferTooSmall, 0};
	const auto operation = static_cast<Operation>(std::to_integer<std::uint8_t>(body[1]));
	const auto id = get32(body, 2);
	const auto payload = body.subspan(6);
	ts::DispatchStatus dispatch = ts::DispatchStatus::InvalidPayload;
	std::uint8_t endpointStatus = 0;
	std::size_t written = 0;
	switch (operation) {
		case Operation::Read:
			if (payload.empty()) {
				const auto result =
				    ts::readFieldEncoded(modelView, id, response.subspan(3), workspace);
				dispatch = result.dispatch;
				written = result.written;
			}
			break;
		case Operation::Write: {
			const auto result = ts::writeFieldEncoded(modelView, id, payload, workspace);
			dispatch = result.dispatch;
			if (dispatch == ts::DispatchStatus::Ok)
				endpointStatus = static_cast<std::uint8_t>(result.endpointStatus);
			break;
		}
		case Operation::Command: {
			const auto result = ts::executeCommandEncoded(modelView, id, payload, workspace);
			dispatch = result.dispatch;
			if (dispatch == ts::DispatchStatus::Ok)
				endpointStatus = static_cast<std::uint8_t>(result.endpointStatus);
			break;
		}
		case Operation::Service: {
			const auto result =
			    ts::callServiceEncoded(modelView, id, payload, response.subspan(3), workspace);
			dispatch = result.dispatch;
			if (dispatch == ts::DispatchStatus::Ok)
				endpointStatus = static_cast<std::uint8_t>(result.endpointStatus);
			written = result.written;
			break;
		}
	}
	response[0] = static_cast<std::byte>(Route::Direct);
	response[1] = static_cast<std::byte>(dispatch);
	response[2] = static_cast<std::byte>(endpointStatus);
	return {PacketStatus::Replied, 3 + written};
}

void StreamReceiver::reset() noexcept
{
	prefixUsed_ = bodyUsed_ = expected_ = 0;
	failure_ = ReceiveStatus::Ok;
}

// Prefix and body counters advance independently across arbitrary chunk
// boundaries. Reset completed frame state before invoking the borrowed sink.
ReceiveStatus StreamReceiver::feed(Input chunk, FrameSink sink, void* context) noexcept
{
	if (failure_ != ReceiveStatus::Ok)
		return failure_;
	if (sink == nullptr)
		return ReceiveStatus::InvalidSink;
	while (!chunk.empty()) {
		if (prefixUsed_ != prefix_.size()) {
			const auto count = std::min(prefix_.size() - prefixUsed_, chunk.size());
			std::copy_n(chunk.begin(), count, prefix_.begin() + prefixUsed_);
			prefixUsed_ += count;
			chunk = chunk.subspan(count);
			if (prefixUsed_ != prefix_.size())
				continue;
			expected_ = std::to_integer<unsigned char>(prefix_[0]) |
			            (std::size_t{std::to_integer<unsigned char>(prefix_[1])} << 8);
			if (expected_ == 0 || expected_ > MaxBodyBytes)
				return failure_ = ReceiveStatus::InvalidLength;
		}
		const auto count = std::min(expected_ - bodyUsed_, chunk.size());
		std::copy_n(chunk.begin(), count, body_.begin() + bodyUsed_);
		bodyUsed_ += count;
		chunk = chunk.subspan(count);
		if (bodyUsed_ != expected_)
			continue;
		const auto reply =
		    onCompletePacket(Input{body_}.first(expected_), Output{response_}.subspan(2));
		if (reply.written == 0)
			return failure_ = ReceiveStatus::ReplyFailure;
		response_[0] = static_cast<std::byte>(reply.written & 0xffu);
		response_[1] = static_cast<std::byte>(reply.written >> 8);
		++completed_;
		prefixUsed_ = bodyUsed_ = expected_ = 0;
		sink(context, Input{response_}.first(reply.written + 2));
	}
	return ReceiveStatus::Ok;
}
} // namespace app::api

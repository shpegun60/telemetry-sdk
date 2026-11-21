/*
 * Runtime application facade and bounded stream receiver.
 *
 * Keep binding templates private while exposing typed local operations and
 * complete-packet routing. The application serializes this facade and keeps
 * borrowed sink context alive for the synchronous callback.
 *
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */

#ifndef TELEMETRY_EXAMPLES_DEVICE_INTEGRATION_API_HPP
#define TELEMETRY_EXAMPLES_DEVICE_INTEGRATION_API_HPP
#pragma once

#include "Device.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace app::api {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;

// IDs are positional: group in the high 16 bits, entry in the low 16 bits.
// The operation selects the Field, Command or Service family. Api.cpp checks
// each against the ID of its path (idOf<model, "device/Period">()).
enum class FieldId : std::uint32_t {
	Period = 0,
	Mode = 1,
	Gains = 2,
	Display = 3
};
enum class CommandId : std::uint32_t {
	Reset = 0,
	Configure = 1
};
enum class ServiceId : std::uint32_t {
	Sample = 0,
	Query = 1
};
enum class Route : std::uint8_t {
	Error = 0,
	Direct = 1,
	Resource = 2
};
enum class Operation : std::uint8_t {
	Read = 1,
	Write = 2,
	Command = 3,
	Service = 4
};
enum class Dispatch : std::uint8_t {
	Ok = 0,
	NotFound = 4,
	InvalidPayload = 5,
	BufferTooSmall = 6,
	WorkspaceTooSmall = 7,
	InternalError = 8,
	Unavailable = 9
};
enum class ServiceStatus : std::uint8_t {
	Ok = 0,
	InvalidArgument = 1
};
enum class PacketStatus {
	Replied,
	InvalidPacket,
	BufferTooSmall
};

struct PacketReply {
	PacketStatus status;
	std::size_t written;
};

struct Diagnostics {
	Counters device;
	unsigned fileReads, fileWrites;
};

// Native reads preserve the supplied output on failure. These operations
// and diagnostics use the same application owner as complete-packet routing.
bool readPeriod(std::uint32_t& value) noexcept;
WriteStatus writePeriod(std::uint32_t value) noexcept;
bool readMode(Mode& value) noexcept;
WriteStatus writeMode(Mode value) noexcept;
bool readGains(Gains& value) noexcept;
WriteStatus writeGains(const Gains& value) noexcept;
bool readDisplay(DisplayConfig& value) noexcept;
WriteStatus writeDisplay(const DisplayConfig& value) noexcept;
Diagnostics diagnostics() noexcept;

// Receives a COMPLETE body without the application's length prefix.
// All calls and diagnostics must be serialized by the application.
// The response is encoded into caller-owned storage before returning.
PacketReply onCompletePacket(Input body, Output response) noexcept;

inline constexpr std::size_t MaxBodyBytes = 128;
// Sink must consume/copy the complete framed reply synchronously. It must not
// retain this borrowed span or recursively call feed() on the same receiver.
using FrameSink = void (*)(void* context, Input frame) noexcept;
enum class ReceiveStatus {
	Ok,
	InvalidLength,
	InvalidSink,
	ReplyFailure
};

// One receiver belongs to one serialized stream. A framing/reply failure
// latches until reset(); an incomplete frame never reaches Device methods.
// Public methods:
// - feed(): Assemble complete frames.
// - reset(): Discard partial input.
// - completedFrames(): Count completed frames.
// - failed(): Inspect latched failure.
class StreamReceiver {
public:
	// The frame is [u16 LE body length][body], length in 1..MaxBodyBytes.
	ReceiveStatus feed(Input chunk, FrameSink sink, void* context) noexcept;
	// Call at a new connection/message-stream boundary or after a timeout.
	// This discards incomplete input; it never resets Device business state.
	void reset() noexcept;

	std::size_t completedFrames() const noexcept
	{
		return completed_;
	}

	bool failed() const noexcept
	{
		return failure_ != ReceiveStatus::Ok;
	}

private:
	std::array<std::byte, 2> prefix_{};
	std::array<std::byte, MaxBodyBytes> body_{};
	std::array<std::byte, MaxBodyBytes + 2> response_{};
	std::size_t prefixUsed_ = 0, bodyUsed_ = 0, expected_ = 0, completed_ = 0;
	ReceiveStatus failure_ = ReceiveStatus::Ok;
};
} // namespace app::api

#endif // TELEMETRY_EXAMPLES_DEVICE_INTEGRATION_API_HPP

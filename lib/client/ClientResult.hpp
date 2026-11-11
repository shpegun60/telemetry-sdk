/* Portable asynchronous client results. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#ifndef TELEMETRY_LIB_TELEMETRY_CLIENT_CLIENTRESULT_HPP
#define TELEMETRY_LIB_TELEMETRY_CLIENT_CLIENTRESULT_HPP
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace telemetry::client {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
enum class Operation : std::uint8_t {
	FieldWrite = 1,
	Command = 2,
	Service = 3,
	FieldRead = 4,
	ReadMany = 5,
	Batch = 6,
	// A raw Resource List, Stat or Read on the same queue, for the device's
	// files. Its payload is the complete Resource request; it has no endpoint.
	// Client-only: it never appears in an Encoded header, and 7 stays reserved
	// in the Encoded operation space so no future operation collides with it.
	Resource = 7
};

[[nodiscard]] constexpr bool isReadOnly(Operation operation) noexcept
{
	return operation == Operation::FieldRead || operation == Operation::ReadMany ||
	       operation == Operation::Resource;
}
enum class Status : std::uint8_t {
	Ok,
	QueueFull,
	PayloadFull,
	NotAdmitted,
	ReadOnly,
	Closed,
	Cancelled,
	Timeout,
	SessionLost,
	OutcomeUnknown,
	MalformedReply,
	TransportError,
	CodecError,
	BufferTooSmall,
	InvalidArgument,
	Busy,
	RecoveryRequired,
	IdsExhausted,
	Unsupported
};

// Status describes client/transport/decode completion. Dispatch and endpoint
// status retain their independent protocol values. All byte/value views borrow
// storage only until the completion callback returns.
struct ResultView {
	Status status = Status::Ok;
	Operation operation = Operation::FieldRead;
	std::uint8_t dispatch = 0;
	std::uint8_t endpointStatus = 0;
	std::uint8_t codec = 0;
	Input payload{};

	[[nodiscard]] bool succeeded() const noexcept
	{
		return status == Status::Ok && dispatch == 0 &&
		       (endpointStatus == 0 || (operation == Operation::Command && endpointStatus == 1));
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return succeeded();
	}

	[[nodiscard]] bool executed() const noexcept
	{
		return status == Status::Ok && dispatch == 0 && operation == Operation::Command &&
		       endpointStatus == 0;
	}

	[[nodiscard]] bool accepted() const noexcept
	{
		return status == Status::Ok && dispatch == 0 && operation == Operation::Command &&
		       endpointStatus == 1;
	}
};

template<class T>
struct Result : ResultView {
	const T* object = nullptr;

	[[nodiscard]] bool hasValue() const noexcept
	{
		return succeeded() && object != nullptr;
	}

	[[nodiscard]] const T& value() const noexcept
	{
		if (!hasValue())
			std::abort();
		return *object;
	}
};

template<>
struct Result<void> : ResultView {};

// A handle is scoped to one live client instance, not only a reusable slot.
struct RequestHandle {
	const void* owner = nullptr;
	std::uint64_t sequence = 0;
	std::uint32_t generation = 0;
	friend bool operator==(RequestHandle, RequestHandle) = default;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return owner && sequence;
	}
};

struct Submission {
	Status status = Status::InvalidArgument;
	RequestHandle handle{};

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}

	[[nodiscard]] RequestHandle value() const noexcept
	{
		return handle;
	}
};

// The function and its context must outlive the client. With multiple producers,
// read() may run concurrently in submission calls and on the owner thread.
struct Clock {
	void* context = nullptr;
	std::uint32_t (*read)(void*) noexcept = nullptr;

	[[nodiscard]] std::uint32_t now() const noexcept
	{
		return read(context);
	}
};

inline constexpr bool expired(std::uint32_t now, std::uint32_t deadline) noexcept
{
	return (now - deadline) < UINT32_C(0x80000000);
}

inline constexpr std::uint32_t maxTimeoutMs = UINT32_C(0x7fffffff);
} // namespace telemetry::client
#endif // TELEMETRY_LIB_TELEMETRY_CLIENT_CLIENTRESULT_HPP

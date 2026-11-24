/**
 * @file Protocol.hpp
 * @brief Read-only synchronous demonstration Model and native protocol dispatch.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_EXAMPLES_NUCLEO_H7S3L8_PROTOCOL_HPP
#define TELEMETRY_EXAMPLES_NUCLEO_H7S3L8_PROTOCOL_HPP
#pragma once
#include <integration/encoded/Dispatcher.hpp>
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <resource/BytesFile.hpp>
#include <resource/FileSystem.hpp>
#include <resource/protocol/Protocol.hpp>
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <array>
#include <cstdint>

#ifndef TELEMETRY_NUCLEO_READ_MANY
#define TELEMETRY_NUCLEO_READ_MANY 0
#endif

namespace nucleo_demo {
struct Voltage {
	float Ua, Ub, Uc;
};
enum class Mode : std::int16_t {
	Idle = -7,
	Active = 12
};

struct Probe {
	std::array<std::array<std::int32_t, 3>, 2> Matrix;
	bool Enabled;
	Mode mode;
};

struct Status {
	std::uint32_t uptimeMs, requests, fieldReads, refused;
	bool demonstration;
};

inline Status status{0, 0, 0, 0, true};

inline Voltage voltage() noexcept
{
	return {230.1f, 229.8f, 231.2f};
}

inline std::uint64_t counter() noexcept
{
	return UINT64_MAX;
}

inline const Status& readStatus() noexcept
{
	return status;
}

inline Probe probe() noexcept
{
	return {{{{1, -2, 3}, {4, 5, -6}}}, true, static_cast<Mode>(-2)};
}
} // namespace nucleo_demo

namespace telemetry::reflection {
template<>
struct EnumReflection<nucleo_demo::Mode> {
	inline static constexpr auto entries = enumEntries(
	    enumEntry(nucleo_demo::Mode::Idle, "Idle"), enumEntry(nucleo_demo::Mode::Active, "Active"));
};
} // namespace telemetry::reflection

namespace nucleo_demo {
#if TELEMETRY_NUCLEO_READ_MANY
// Stable native owners, with position-encoding values for exact wire checks.
// Counters measure actual scalar getter execution, independently of dispatch.
struct ScalarFixture {
	std::uint32_t calls = 0;

	template<std::size_t I>
	std::uint32_t u32() noexcept
	{
		++calls;
		return 0x10203040u + std::uint32_t{I} * 0x01010101u;
	}

	template<std::size_t I>
	std::uint64_t u64() noexcept
	{
		++calls;
		return UINT64_MAX - std::uint64_t{I} * UINT64_C(0x0102030405060708);
	}

	template<std::size_t I>
	bool flag() noexcept
	{
		++calls;
		return I % 2 == 0;
	}

	template<std::size_t I>
	Mode mode() noexcept
	{
		++calls;
		return static_cast<Mode>(-2 - static_cast<std::int16_t>(I));
	}
};

inline ScalarFixture scalarFixture;
inline telemetry::OwnerSlot<ScalarFixture> unavailableFixture;
#endif
inline constexpr auto measure = telemetry::group("measure", telemetry::field<&voltage>("Voltage"));
inline constexpr auto device =
    telemetry::group("device", telemetry::field<&counter>("Counter"),
                     telemetry::field<&readStatus>("Status"), telemetry::field<&probe>("Probe"));
#if TELEMETRY_NUCLEO_READ_MANY
inline constexpr auto batch32 =
    telemetry::group("batch32", telemetry::field<&ScalarFixture::u32<0>>("U32_0", scalarFixture),
                     telemetry::field<&ScalarFixture::u32<1>>("U32_1", scalarFixture),
                     telemetry::field<&ScalarFixture::u32<2>>("U32_2", scalarFixture),
                     telemetry::field<&ScalarFixture::u32<3>>("U32_3", scalarFixture),
                     telemetry::field<&ScalarFixture::u32<4>>("U32_4", scalarFixture),
                     telemetry::field<&ScalarFixture::u32<5>>("U32_5", scalarFixture),
                     telemetry::field<&ScalarFixture::u32<6>>("U32_6", scalarFixture),
                     telemetry::field<&ScalarFixture::u32<7>>("U32_7", scalarFixture));
inline constexpr auto batch64 =
    telemetry::group("batch64", telemetry::field<&ScalarFixture::u64<0>>("U64_0", scalarFixture),
                     telemetry::field<&ScalarFixture::u64<1>>("U64_1", scalarFixture),
                     telemetry::field<&ScalarFixture::u64<2>>("U64_2", scalarFixture),
                     telemetry::field<&ScalarFixture::u64<3>>("U64_3", scalarFixture),
                     telemetry::field<&ScalarFixture::u64<4>>("U64_4", scalarFixture),
                     telemetry::field<&ScalarFixture::u64<5>>("U64_5", scalarFixture),
                     telemetry::field<&ScalarFixture::u64<6>>("U64_6", scalarFixture),
                     telemetry::field<&ScalarFixture::u64<7>>("U64_7", scalarFixture));
inline constexpr auto flags =
    telemetry::group("flags", telemetry::field<&ScalarFixture::flag<0>>("Bool_0", scalarFixture),
                     telemetry::field<&ScalarFixture::flag<1>>("Bool_1", scalarFixture),
                     telemetry::field<&ScalarFixture::flag<2>>("Bool_2", scalarFixture),
                     telemetry::field<&ScalarFixture::flag<3>>("Bool_3", scalarFixture),
                     telemetry::field<&ScalarFixture::flag<4>>("Bool_4", scalarFixture),
                     telemetry::field<&ScalarFixture::flag<5>>("Bool_5", scalarFixture),
                     telemetry::field<&ScalarFixture::flag<6>>("Bool_6", scalarFixture),
                     telemetry::field<&ScalarFixture::flag<7>>("Bool_7", scalarFixture));
inline constexpr auto modes =
    telemetry::group("modes", telemetry::field<&ScalarFixture::mode<0>>("Mode_0", scalarFixture),
                     telemetry::field<&ScalarFixture::mode<1>>("Mode_1", scalarFixture),
                     telemetry::field<&ScalarFixture::mode<2>>("Mode_2", scalarFixture),
                     telemetry::field<&ScalarFixture::mode<3>>("Mode_3", scalarFixture),
                     telemetry::field<&ScalarFixture::mode<4>>("Mode_4", scalarFixture),
                     telemetry::field<&ScalarFixture::mode<5>>("Mode_5", scalarFixture),
                     telemetry::field<&ScalarFixture::mode<6>>("Mode_6", scalarFixture),
                     telemetry::field<&ScalarFixture::mode<7>>("Unavailable", unavailableFixture));
// Existing four IDs are unchanged. Counter plus 31 new getters are exactly
// 32 readable scalar Field IDs; Unavailable is a separate scalar-typed row.
inline constexpr telemetry::Model model{measure, device, batch32, batch64, flags, modes};
#else
inline constexpr telemetry::Model model{measure, device};
#endif
// The IDs a host reads, from their paths: groups are numbered in tree order.
static_assert(telemetry::idOf<model, "measure/Voltage">() == 0x00000000u);
static_assert(telemetry::idOf<model, "device/Counter">() == 0x00010000u);
static_assert(telemetry::idOf<model, "device/Probe">() == 0x00010002u);
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto packedDescriptor = telemetry::descriptor::packDescriptor<descriptor>();
inline constexpr resource::telemetry::v3::DescriptorFile descriptorFile{packedDescriptor};
inline constexpr std::array<std::byte, 4> versionBytes{std::byte{1}, std::byte{0}, std::byte{0},
                                                       std::byte{0}};
inline constexpr resource::BytesFile versionFile{versionBytes};
inline constexpr auto files =
    resource::filesystem(resource::file("/device/version.bin", versionFile),
                         resource::file("/telemetry/descriptor.bin", descriptorFile));

class Protocol {
public:
#if TELEMETRY_NUCLEO_READ_MANY
	// A restored last epoch can be supplied by an application. The demo owns
	// one Protocol for its whole run and starts at zero (not yet admitted).
	explicit Protocol(std::uint32_t retiredEpoch = 0) noexcept : epoch_(retiredEpoch)
	{}
#else
	Protocol() noexcept = default;
#endif
	Protocol(const Protocol&) = delete;
	Protocol& operator=(const Protocol&) = delete;
	Protocol(Protocol&&) = delete;
	Protocol& operator=(Protocol&&) = delete;

	[[nodiscard]] std::size_t dispatch(resource::Input input, resource::Output output,
	                                   std::uint32_t now) noexcept
	{
		status.uptimeMs = now;
		++status.requests;
		if (input.empty())
			return refuse();
		const auto code = std::to_integer<unsigned>(input[0]);
		if (code >= 1 && code <= 3) {
#if TELEMETRY_NUCLEO_READ_MANY
			bool initial = code == 1 && input.size() == resource::protocol::wire::listRequestSize;
			for (std::size_t i = 1; initial && i < input.size(); ++i)
				initial = input[i] == std::byte{0};
			const auto result = resource::protocol::process(files_.view(), input, output);
			// Resource validates the complete request and output capacity first.
			// LIST cursor 0 commits a discovery boundary only after a successful
			// reply containing at least one whole path. STAT/READ never rotate it.
			if (initial && result.status == resource::Status::Ok &&
			    result.written > resource::protocol::wire::chunkReplyHeaderSize)
				advanceEpoch();
#else
			const auto result = resource::protocol::process(files.view(), input, output);
#endif
			return result.written;
		}
		const auto request = telemetry::protocol::encoded::parse(input);
		if (!request || request.header.reply ||
		    (request.header.operation != telemetry::protocol::encoded::Operation::FieldRead
#if TELEMETRY_NUCLEO_READ_MANY
		     && request.header.operation != telemetry::protocol::encoded::Operation::ReadMany
#endif
		     ))
			return refuse();
#if TELEMETRY_NUCLEO_READ_MANY
		if (capability_.session == 0 || request.header.session != capability_.session)
			return refuse();
#endif
		++status.fieldReads;
#if TELEMETRY_NUCLEO_READ_MANY
		// Both read operations belong to the last committed discovery epoch.
		// Calls remain synchronous, sequential and do not promise a snapshot.
		const auto result = telemetry::integration::encoded::dispatchEncoded(
		    model.view(), workspace_, capability_.session, input, output, capability_);
#else
		// No remote session state or mailbox: every read is synchronous. The
		// exclusive host supplies and correlates a fresh nonzero TEQ1 session.
		const auto result = telemetry::integration::encoded::dispatchEncoded(
		    model.view(), workspace_, request.header.session, input, output);
#endif
		return result ? result.written : refuse();
	}

private:
#if TELEMETRY_NUCLEO_READ_MANY
	void advanceEpoch() noexcept
	{
		if (epoch_ == UINT32_MAX) {
			capability_ = {};
			capabilityBytes_.fill(std::byte{0});
			capabilityFile_ = resource::BytesFile{resource::Input{}};
			return; // Never wrap or reuse an exhausted session.
		}
		++epoch_;
		capability_ = {epoch_, descriptor.fingerprint()};
		(void)telemetry::protocol::encoded::read_many::writeCapability(capabilityBytes_,
		                                                               capability_);
		capabilityFile_ = resource::BytesFile{capabilityBytes_};
	}

	std::uint32_t epoch_ = 0;
	telemetry::protocol::encoded::read_many::Capability capability_{};
	std::array<std::byte, telemetry::protocol::encoded::read_many::capabilityBytes>
	    capabilityBytes_{};
	resource::BytesFile capabilityFile_{resource::Input{}};
	// File descriptors borrow this Protocol's provider; copying/moving would
	// leave dangling self-pointers, so its address stays fixed for its lifetime.
	resource::FileSystem<3> files_{
	    resource::filesystem(resource::file("/device/version.bin", versionFile),
	                         resource::file("/telemetry/descriptor.bin", descriptorFile),
	                         resource::file("/telemetry/read-many.bin", capabilityFile_))};
#endif
	std::size_t refuse() noexcept
	{
		++status.refused;
		return 0;
	}

	alignas(std::max_align_t) std::array<std::byte, model.maxScratch()> scratch_{};
	telemetry::Workspace workspace_{scratch_};
};
} // namespace nucleo_demo
#endif // TELEMETRY_EXAMPLES_NUCLEO_H7S3L8_PROTOCOL_HPP

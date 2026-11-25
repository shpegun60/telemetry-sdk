/*
 * @file Standalone.cpp
 * @brief Generated canonical bytes, bounded storage and optional native interop.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <sdk/device/Codec.hpp>
#ifdef CODEGEN_WITH_TELEMETRY
#include <sdk/device/TelemetryBindings.hpp>
#include <sdk/device/RpcBindings.hpp>
#include <telemetry/codec/Codec.hpp>
#endif
#include <cstdio>
#include <cstring>
#include <limits>

namespace api = sdk::device;
using Value = api::motor::types::Config;
static unsigned checks = 0, failures = 0;

static void check(bool value, unsigned line) noexcept
{
	++checks;
	if (!value) {
		++failures;
		std::fprintf(stderr, "Standalone.cpp:%u\n", line);
	}
}

#define CHECK(value) check(bool(value), __LINE__)
static Value input{UINT64_MAX,
                   INT64_MIN,
                   true,
                   -0.0f,
                   -0.0,
                   static_cast<api::detail::Type_12>(42),
                   api::detail::Type_13::Min,
                   {{1.0f, -2.0f, std::numeric_limits<float>::infinity()}},
                   {},
                   {}};
static Value output{};
static std::array<std::byte, api::codec::wireSize<Value>> wire{};
static api::motor::types::LargeRequest large{};
static std::array<std::byte, 10001> largeWire{};

int main()
{
	using S = api::codec::Status;
	static_assert(api::codec::wireSize<Value> == 57);
	CHECK(api::schema::matchesHeader(api::schema::descriptorHeader));
	for (std::size_t i = 0; i < 64; ++i) {
		auto header = api::schema::descriptorHeader;
		header[i] ^= std::byte{1};
		CHECK(!api::schema::matchesHeader(header));
	}
	CHECK(!api::schema::matchesHeader(std::span(api::schema::descriptorHeader).first(63)));
	CHECK(api::codec::encode(api::motor::Apply, input, wire) == S::Ok);
	CHECK(wire[0] == std::byte{255} && wire[15] == std::byte{128} && wire[16] == std::byte{1});
	CHECK(api::codec::decodeResponse(api::motor::Read, wire, output) == S::Ok);
	CHECK(output.unsignedValue == UINT64_MAX && output.signedValue == INT64_MIN && output.valid);
	CHECK(std::bit_cast<std::uint32_t>(output.single) == 0x80000000u);
	CHECK(std::bit_cast<std::uint64_t>(output.real) == 0x8000000000000000ULL);
	CHECK(static_cast<std::uint64_t>(output.code) == 42 &&
	      output.signedCode == api::detail::Type_13::Min);
	std::array<std::byte, sizeof(Value)> snapshot{};
	std::memcpy(snapshot.data(), &output, sizeof output);
	wire[16] = std::byte{2};
	CHECK(api::codec::decode(wire, output) == S::InvalidValue);
	CHECK(std::memcmp(snapshot.data(), &output, sizeof output) == 0);
	CHECK(api::codec::decode(std::span(wire).first(56), output) == S::LengthMismatch);
	CHECK(api::codec::encode(output, std::as_writable_bytes(std::span(&output, 1)).first(57)) ==
	      S::Overlap);
	CHECK(api::codec::decode(std::as_bytes(std::span(&output, 1)).first(57), output) == S::Overlap);
	CHECK(api::codec::encode(api::motor::Reset, {}) == S::Ok);
	CHECK(api::codec::encode(api::motor::Read, {}) == S::Ok);
	CHECK(api::codec::decodeResponse(api::motor::Noop, {}) == S::Ok);
	CHECK(api::codec::encode(api::motor::Large, large, largeWire) == S::Ok);
	largeWire[10000] = std::byte{2};
	CHECK(api::codec::decode(largeWire, large) == S::InvalidValue && !large.valid);
	// Nested aggregate/array cursors must advance exactly once at each boundary.
	api::motor::types::Nested nested{}, nestedRestored{};
	nested.prefix = 0xab;
	nested.rows[0] = {0x1234, {true, false, true}, false};
	nested.rows[1] = {0x5678, {false, true, false}, true};
	nested.suffix = 0x12345678;
	nested.valid = true;
	constexpr std::array<std::byte, 18> expected{
	    std::byte{0xab}, std::byte{0x34}, std::byte{0x12}, std::byte{1},    std::byte{0},
	    std::byte{1},    std::byte{0},    std::byte{0x78}, std::byte{0x56}, std::byte{0},
	    std::byte{1},    std::byte{0},    std::byte{1},    std::byte{0x78}, std::byte{0x56},
	    std::byte{0x34}, std::byte{0x12}, std::byte{1}};
	std::array<std::byte, 18> nestedWire{};
	CHECK(api::codec::encode(nested, nestedWire) == S::Ok && nestedWire == expected);
	CHECK(api::codec::decode(nestedWire, nestedRestored) == S::Ok);
	CHECK(nestedRestored.prefix == nested.prefix && nestedRestored.rows[1].tag == 0x5678 &&
	      nestedRestored.rows[0].flags[2] && !nestedRestored.rows[1].flags[2] &&
	      nestedRestored.suffix == nested.suffix && nestedRestored.valid);
	std::array<std::byte, sizeof(nestedRestored)> nestedSnapshot{};
	std::memcpy(nestedSnapshot.data(), &nestedRestored, sizeof nestedRestored);
	for (const auto offset : {3u, 4u, 5u, 6u, 9u, 10u, 11u, 12u, 17u}) {
		auto invalid = expected;
		invalid[offset] = std::byte{2};
		CHECK(api::codec::decode(invalid, nestedRestored) == S::InvalidValue);
		CHECK(std::memcmp(nestedSnapshot.data(), &nestedRestored, sizeof nestedRestored) == 0);
	}
#ifdef CODEGEN_WITH_TELEMETRY
	std::array<std::byte, 57> generated{}, native{};
	CHECK(api::codec::encode(input, generated) == S::Ok);
	CHECK(telemetry::encode(input, native) == telemetry::CodecStatus::Ok && native == generated);
	Value decoded{};
	CHECK(telemetry::decode(generated, decoded) == telemetry::CodecStatus::Ok);
	CHECK(decoded.signedCode == api::detail::Type_13::Min);
	static_assert(telemetry::reflection::Enum<api::detail::Type_13>::entryCount == 2);
	static_assert(telemetry::reflection::Enum<api::detail::Type_20>::entryCount == 0);
	api::detail::Type_20 sparse = static_cast<api::detail::Type_20>(-1234567), restored{};
	std::array<std::byte, 4> code{};
	CHECK(telemetry::encode(sparse, code) == telemetry::CodecStatus::Ok);
	CHECK(api::codec::decode(code, restored) == S::Ok && restored == sparse);
	std::array<std::byte, 8> edgeGenerated{}, edgeNative{};
	for (auto edge : {api::detail::Type_12::Zero, api::detail::Type_12::Max}) {
		CHECK(api::codec::encode(edge, edgeGenerated) == S::Ok);
		CHECK(telemetry::encode(edge, edgeNative) == telemetry::CodecStatus::Ok &&
		      edgeGenerated == edgeNative);
	}
	for (auto edge : {api::detail::Type_13::Min, api::detail::Type_13::Max}) {
		CHECK(api::codec::encode(edge, edgeGenerated) == S::Ok);
		CHECK(telemetry::encode(edge, edgeNative) == telemetry::CodecStatus::Ok &&
		      edgeGenerated == edgeNative);
	}
	const auto binding =
	    api::rpc::binding(api::motor::Apply, ::telemetry::client::rpc::trustedRpcMapping);
	CHECK(binding.id == api::motor::Apply.id);
#endif
	std::printf("codegen standalone checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}

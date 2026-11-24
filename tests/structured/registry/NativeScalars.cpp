/*
 * @file NativeScalars.cpp
 * @brief Native integer spellings produce canonical Descriptor v3 scalar IDs.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/runtime/DynamicValueView.hpp>
#include <array>
#include <cstdio>
#include <limits>

namespace fixture {
enum class Code : long long {
	Negative = -23,
	Positive = 99
};

struct Integers {
	signed char i8;
	unsigned char u8;
	short i16;
	unsigned short u16;
	int i32;
	unsigned u32;
	long nativeLong;
	unsigned long nativeUnsignedLong;
	long long i64;
	unsigned long long u64;
	Code code;
	std::array<std::array<long, 2>, 2> nested;
};

inline constexpr Integers input{-7,
                                249,
                                -12345,
                                54321,
                                -1234567,
                                3456789,
                                -314159,
                                271828,
                                std::numeric_limits<long long>::min(),
                                std::numeric_limits<unsigned long long>::max(),
                                Code::Negative,
                                {{{-1, 2}, {3, -4}}}};

Integers read() noexcept
{
	return input;
}

long long scalar() noexcept
{
	return input.i64;
}

inline constexpr telemetry::Model model{telemetry::group(
    "native", telemetry::field<&read>("Integers"), telemetry::field<&scalar>("Scalar"))};
using Registry = typename std::remove_cvref_t<decltype(model)>::Registry;
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = telemetry::descriptor::packDescriptor<descriptor>();
static_assert(model.typeId<long long>() == 9);
static_assert(model.typeId<unsigned long long>() == 8);
static_assert(Registry::descriptor<model.typeId<Code>()>().relatedTypeId == 9);
static_assert(Registry::descriptor<model.typeId<std::array<long, 2>>()>().relatedTypeId ==
              model.typeId<long>());
} // namespace fixture

static bool save(const char* path, std::span<const std::byte> value)
{
	auto* file = std::fopen(path, "wb");
	if (!file)
		return false;
	const bool written = std::fwrite(value.data(), 1, value.size(), file) == value.size();
	return std::fclose(file) == 0 && written;
}

int main(int argc, char** argv)
{
	using namespace telemetry;
	using namespace telemetry::runtime;
	std::array<RuntimeType, 32> types{};
	std::array<RuntimeMember, 32> members{};
	std::array<RuntimeEnumEntry, 8> enums{};
	std::array<RuntimeCatalog, 4> catalogs{};
	std::array<RuntimeEndpoint, 4> endpoints{};
	RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	const auto parsed = RuntimeSchema::parse(fixture::bytes, workspace);
	if (!parsed)
		return 1;
	std::array<std::byte, wireSize<fixture::Integers>> encoded{};
	if (encode(fixture::input, encoded) != CodecStatus::Ok)
		return 2;
	fixture::Integers decoded{};
	if (decode(encoded, decoded) != CodecStatus::Ok || decoded.i8 != -7 || decoded.u8 != 249 ||
	    decoded.i16 != -12345 || decoded.u16 != 54321 || decoded.i32 != -1234567 ||
	    decoded.u32 != 3456789 || decoded.nativeLong != -314159 ||
	    decoded.nativeUnsignedLong != 271828 || decoded.i64 != fixture::input.i64 ||
	    decoded.u64 != fixture::input.u64 || decoded.code != fixture::Code::Negative ||
	    decoded.nested != fixture::input.nested)
		return 3;
	const auto value = parsed.schema.view(fixture::model.typeId<fixture::Integers>(), encoded);
	if (!value)
		return 4;
	const auto i64 = value.value.member("i64");
	const auto u64 = value.value.member("u64");
	const auto code = value.value.member("code");
	const auto nested = value.value.member("nested");
	if (!i64 || !u64 || !code || !nested)
		return 5;
	const auto signedValue = i64.value.get<std::int64_t>();
	const auto unsignedValue = u64.value.get<std::uint64_t>();
	const auto enumValue = code.value.enumUnderlying<std::int64_t>();
	const auto enumName = code.value.enumName();
	if (!signedValue || signedValue.value != fixture::input.i64 || !unsignedValue ||
	    unsignedValue.value != fixture::input.u64 || !enumValue || enumValue.value != -23 ||
	    !enumName || !enumName.value.known || enumName.value.name != "Negative")
		return 6;
	const auto row = nested.value.index(1);
	const auto cell = row ? row.value.index(1) : Result<DynamicValueView>{};
	if (!cell)
		return 7;
	using CanonicalLong = std::conditional_t<sizeof(long) == 8, std::int64_t, std::int32_t>;
	const auto longValue = cell.value.get<CanonicalLong>();
	if (!longValue || longValue.value != -4)
		return 8;
	for (std::uint32_t i = 12; i < fixture::model.types().count; ++i)
		if (fixture::model.types().find(i)->kind == TypeKind::Scalar)
			return 9;
	if (argc == 3 && (!save(argv[1], fixture::bytes) || !save(argv[2], encoded)))
		return 10;
	std::printf(
	    "Native scalar Descriptor/Codec/Runtime roundtrip passed: long=%zu types=%u bytes=%zu\n",
	    sizeof(long), fixture::model.types().count, encoded.size());
}

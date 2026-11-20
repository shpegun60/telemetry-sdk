/* Descriptor/Values fixtures: whole tokens, unavailable slots, exact bits and 4 KiB reads. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */

#ifndef TELEMETRY_TESTS_STRUCTURED_RESOURCES_FIXTURE_HPP
#define TELEMETRY_TESTS_STRUCTURED_RESOURCES_FIXTURE_HPP
#pragma once

#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <resource/FileSystem.hpp>
#include <bit>

namespace fixture {
namespace ts = telemetry;
namespace rs = resource::telemetry::v3;

// Zero-wire-size Field still requires one Values status byte.
struct Empty {};

// Mixed-width Field payload proves canonical bytes omit native padding.
struct Point {
	std::uint16_t code;
	float value;
	bool ready;
};
enum class Mode : std::int16_t {
	Off = -1,
	On = 2
};
inline std::array<unsigned, 12> calls{};
inline std::uint32_t counter = 0x12345678;

inline std::uint32_t readCounter() noexcept
{
	++calls[0];
	return counter;
}

inline bool readBool() noexcept
{
	++calls[1];
	return true;
}

inline double readDouble() noexcept
{
	++calls[2];
	return -0.0;
}

inline Mode readEnum() noexcept
{
	++calls[3];
	return static_cast<Mode>(-2);
}

inline Point readPoint() noexcept
{
	++calls[4];
	return {0x1234, 1.5f, false};
}

inline std::array<std::uint16_t, 3> readArray() noexcept
{
	++calls[5];
	return {1, 256, 65535};
}

inline Empty readEmpty() noexcept
{
	++calls[6];
	return {};
}

inline std::array<std::uint8_t, 0> readZero() noexcept
{
	++calls[7];
	return {};
}

inline std::uint32_t readSlot() noexcept
{
	++calls[8];
	return 42;
}

// Counted late-bound Field owner. API: read() returns Point and records one getter call.
struct Owner {
	Point read() noexcept
	{
		++calls[9];
		return {1, 2.0f, true};
	}
};

inline Owner owner;
inline telemetry::OwnerSlot<Owner> ownerSlot;
inline telemetry::FunctionSlot<std::uint32_t() noexcept> functionSlot;
inline constexpr ts::FieldTable first{ts::field<&readCounter>("Counter")};
inline constexpr ts::FieldTable<> emptyTable{};
inline constexpr ts::FieldTable rest{ts::field<&readBool>("Bool"),
                                     ts::field<&readDouble>("Double"),
                                     ts::field<&readEnum>("Enum"),
                                     ts::field<&readPoint>("Point"),
                                     ts::field<&readArray>("Array"),
                                     ts::field<&readEmpty>("Empty"),
                                     ts::field<&readZero>("Zero"),
                                     ts::field("Function", functionSlot),
                                     ts::field<&Owner::read>("Owner", ownerSlot)};
inline constexpr ts::FieldCatalogTable fields{
    ts::group("first", first), ts::group("empty", emptyTable), ts::group("rest", rest)};
inline constexpr ts::ServiceCatalogTable<> services{};
inline constexpr ts::Model model{fields, ts::emptyCommands, services};
// values.hex is a frozen v3.0 golden whose header copies this descriptor's
// fingerprint: the profile strips type names on every build.
inline constexpr auto descriptor = ::telemetry::descriptor::withoutTypeNames(model);
inline constexpr auto packed = ::telemetry::descriptor::packDescriptor<descriptor>();
inline std::array<std::byte, model.maxFieldScratch()> storage;
inline ts::Workspace workspace{storage};
inline constexpr rs::ValuesFile values{descriptor, workspace};
inline constexpr rs::DescriptorFile packedFile{packed};
inline constexpr rs::DescriptorFile streamedFile{descriptor};
inline constexpr auto fs =
    resource::filesystem(resource::file("/telemetry3/descriptor.bin", packedFile),
                         resource::file("/telemetry3/values.bin", values));

// Explicit starts make this test independent of the provider's offset table.
inline constexpr std::array<std::uint32_t, 11> offsets{24, 29, 31, 40, 43, 51, 58, 59, 60, 65, 73};
static_assert(values.size() == 73 && values.fieldCount() == 10 && values.maxTokenSize() == 9);
static_assert(values.fingerprint() == descriptor.fingerprint());
inline constexpr ts::Model emptyModel{ts::emptyFields, ts::emptyCommands, services};
inline constexpr ::telemetry::descriptor::Descriptor emptyDescriptor{emptyModel};
inline constexpr rs::ValuesFile emptyValues{emptyDescriptor, workspace};
static_assert(emptyValues.size() == 24 && emptyValues.fieldCount() == 0);

using ExactCvEmptyModel =
    ts::Model<decltype(ts::emptyFields), decltype(ts::emptyCommands), decltype(services)>;
inline constexpr ExactCvEmptyModel exactCvEmptyModel{ts::emptyFields, ts::emptyCommands, services};
inline constexpr ::telemetry::descriptor::Descriptor exactCvEmptyDescriptor{exactCvEmptyModel};
inline constexpr rs::ValuesFile exactCvEmptyValues{exactCvEmptyDescriptor, workspace};
static_assert(exactCvEmptyDescriptor.valid() &&
              ::telemetry::descriptor::packDescriptor<exactCvEmptyDescriptor>() ==
                  ::telemetry::descriptor::packDescriptor<emptyDescriptor>());
static_assert(exactCvEmptyValues.size() == emptyValues.size() &&
              exactCvEmptyValues.fieldCount() == 0 && exactCvEmptyValues.requiredWorkspace() == 0 &&
              exactCvEmptyValues.fingerprint() == emptyValues.fingerprint());

using Big = std::array<std::uint32_t, 1024>;

inline Big readBig() noexcept
{
	++calls[10];
	return Big{0x01020304};
}

inline constexpr ts::FieldTable bigTable{ts::field<&readCounter>("Small"),
                                         ts::field<&readBig>("Big")};
inline constexpr ts::FieldCatalogTable bigFields{ts::group("big", bigTable)};
inline constexpr ts::Model bigModel{bigFields, ts::emptyCommands, services};
inline constexpr ::telemetry::descriptor::Descriptor bigDescriptor{bigModel};
inline std::array<std::byte, bigModel.maxFieldScratch()> bigStorage;
inline ts::Workspace bigWorkspace{bigStorage};
inline constexpr rs::ValuesFile bigValues{bigDescriptor, bigWorkspace};
static_assert(bigValues.maxTokenSize() == 4097 && bigValues.requiredWorkspace() == 4099);
} // namespace fixture

#endif // TELEMETRY_TESTS_STRUCTURED_RESOURCES_FIXTURE_HPP

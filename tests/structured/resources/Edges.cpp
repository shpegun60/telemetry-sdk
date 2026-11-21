/* Payload ceilings, exact numeric bits and rejected metadata. MIT. */
#include "Fixture.hpp"
#include <resource/protocol/Protocol.hpp>
#include <algorithm>
#include <cassert>
#include <limits>

namespace {
using namespace fixture;
using Large = std::array<std::uint8_t, 65536>;
unsigned largeCalls = 0;

Large large() noexcept
{
	++largeCalls;
	return {};
}

constexpr ts::Model largeModel{ts::group("large", ts::field<&large>("Large"))};
constexpr ::telemetry::descriptor::Descriptor largeDescriptor{largeModel};
std::array<std::byte, largeModel.maxFieldScratch()> largeStorage;
ts::Workspace largeWorkspace{largeStorage};
constexpr rs::ValuesFile largeValues{largeDescriptor, largeWorkspace};
constexpr auto largeFs = resource::filesystem(resource::file("/large", largeValues));
std::array<std::byte, 65560> buffer;

std::uint64_t u64() noexcept
{
	return UINT64_MAX;
}

std::int64_t s64() noexcept
{
	return INT64_MIN;
}

float nan() noexcept
{
	return std::bit_cast<float>(std::uint32_t{0x7fc01234});
}

constexpr ts::FieldTable numeric{ts::field<&u64>("U64"), ts::field<&s64>("S64"),
                                 ts::field<&nan>("NaN")};
// The catalog engine with deduced and explicit const-qualified arguments.
constexpr fixture::v30::FieldCatalogTable numericFields{fixture::v30::group("numeric", numeric)};
constexpr fixture::v30::V30Model numericModel{numericFields, fixture::v30::emptyCommands, services};
constexpr ::telemetry::descriptor::Descriptor numericDescriptor{numericModel};
std::array<std::byte, numericModel.maxFieldScratch()> numericStorage;
ts::Workspace numericWorkspace{numericStorage};
constexpr rs::ValuesFile numbers{numericDescriptor, numericWorkspace};

using ExactCvNumericModel =
    fixture::v30::V30Model<decltype(numericFields), decltype(fixture::v30::emptyCommands),
                           decltype(services)>;
constexpr ExactCvNumericModel exactCvNumericModel{numericFields, fixture::v30::emptyCommands,
                                                  services};
constexpr ::telemetry::descriptor::Descriptor exactCvNumericDescriptor{exactCvNumericModel};
constexpr rs::ValuesFile exactCvNumbers{exactCvNumericDescriptor, numericWorkspace};
static_assert(exactCvNumericDescriptor.valid() &&
              ::telemetry::descriptor::packDescriptor<exactCvNumericDescriptor>() ==
                  ::telemetry::descriptor::packDescriptor<numericDescriptor>());
static_assert(exactCvNumbers.size() == numbers.size() &&
              exactCvNumbers.requiredWorkspace() == numbers.requiredWorkspace() &&
              exactCvNumbers.maxTokenSize() == numbers.maxTokenSize());
} // namespace

int main()
{
	using resource::Status;
	static_assert(largeValues.maxTokenSize() == 65537);
	std::array<std::byte, 13> request{};
	request[0] = std::byte{3};
	auto r = resource::protocol::process(largeFs.view(), request, buffer);
	assert(r.status == Status::Ok && r.written == 12 + 24 && buffer[1] == std::byte{24});
	request[5] = std::byte{24};
	r = resource::protocol::process(largeFs.view(), request, buffer);
	assert(r.status == Status::BufferTooSmall && r.written == 12 && largeCalls == 0);
	assert(buffer[1] == std::byte{24} && buffer[9] == std::byte{0});
	// The provider itself can serve this field when the caller has capacity.
	assert(largeValues.read(24, buffer).eof && largeCalls == 1);
	assert(largeWorkspace.used() == 0);
	assert(numbers.read(0, buffer).eof);
	assert(buffer[24] == std::byte{0});
	for (unsigned i = 25; i < 33; ++i)
		assert(buffer[i] == std::byte{0xff});
	for (unsigned i = 33; i < 41; ++i)
		assert(buffer[i] == std::byte{0});
	assert(buffer[41] == std::byte{0x80});
	assert(buffer[42] == std::byte{0} && buffer[43] == std::byte{0x34} &&
	       buffer[44] == std::byte{0x12} && buffer[45] == std::byte{0xc0} &&
	       buffer[46] == std::byte{0x7f});

	// Exact-cv catalogs retain the same descriptor and live Values bytes.
	std::array<std::byte, numbers.size()> exactCvOutput{};
	const auto exactRead = exactCvNumbers.read(0, exactCvOutput);
	assert(exactRead.status == Status::Ok && exactRead.eof &&
	       exactRead.written == exactCvOutput.size());
	assert(std::equal(exactCvOutput.begin(), exactCvOutput.end(), buffer.begin()));
	std::array<std::byte, emptyValues.size()> emptyOutput{}, exactCvEmptyOutput{};
	assert(emptyValues.read(0, emptyOutput).eof);
	const auto exactEmptyRead = exactCvEmptyValues.read(0, exactCvEmptyOutput);
	assert(exactEmptyRead.status == Status::Ok && exactEmptyRead.eof &&
	       exactEmptyRead.written == exactCvEmptyOutput.size() &&
	       exactCvEmptyOutput == emptyOutput);
	assert(numericWorkspace.used() == 0 && workspace.used() == 0);

	char invalidName[] = "Duplicate";
	const ts::FieldTable badTable{ts::field<&u64>(invalidName), ts::field<&s64>(invalidName)};
	const ts::FieldCatalogTable badFields{ts::group("bad", badTable)};
	const ts::Model badModel{badFields, ts::emptyCommands, services};
	const ::telemetry::descriptor::Descriptor badDescriptor{badModel};
	assert(!badDescriptor.valid());
	rs::ValuesFile rejected{badDescriptor, workspace};
	assert(rejected.size() == 0 && rejected.fingerprint() == 0);
	assert(rejected.read(0, buffer).status == Status::InvalidData);
}

/* Paired owning/borrowed type identity, descriptor and whole-value resources. MIT. */
#include "Fixture.hpp"
#include "Check.hpp"
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <algorithm>
#include <cstring>
#ifndef BORROWED_ARM
#include <cstdio>
#endif

using namespace borrowed_fixture;
using borrowed_test::check;

namespace wire_fixture {
namespace rs = ::resource::telemetry::v3;
inline Owner<Config> config{{0x12345678, true}};
inline Owner<Blob<4096>> kib4{};
inline Owner<Blob<65536>> kib64{};
inline ts::FunctionSlot<Config() noexcept> absentOwn;
inline ts::FunctionSlot<const Config&() noexcept> absentBorrow;
// Owning and borrowing getters of the same Fields, as group expressions.
inline constexpr auto ownFields =
    ts::group("fields", ts::field<&Owner<Config>::own, &Owner<Config>::set>("Config", config),
              ts::field<&Owner<Blob<4096>>::own, &Owner<Blob<4096>>::set>("Kib4", kib4),
              ts::field<&Owner<Blob<65536>>::own, &Owner<Blob<65536>>::set>("Kib64", kib64),
              ts::field("Absent", absentOwn));
inline constexpr auto borrowFields =
    ts::group("fields", ts::field<&Owner<Config>::get, &Owner<Config>::set>("Config", config),
              ts::field<&Owner<Blob<4096>>::get, &Owner<Blob<4096>>::set>("Kib4", kib4),
              ts::field<&Owner<Blob<65536>>::get, &Owner<Blob<65536>>::set>("Kib64", kib64),
              ts::field("Absent", absentBorrow));

inline ts::ServiceResult<Config> ownService(const Request&) noexcept
{
	return ts::ServiceResult<Config>::success(config.value);
}

inline const Config& borrowService(const Request&) noexcept
{
	return config.value;
}

inline ts::BorrowedServiceResult<Config> wrappedService(const Request&) noexcept
{
	return ts::BorrowedServiceResult<Config>::success(config.value);
}

inline constexpr ts::Model ownModel{ownFields,
                                    ts::group("services", ts::service<&ownService>("Read"))};
inline constexpr ts::Model borrowModel{borrowFields,
                                       ts::group("services", ts::service<&borrowService>("Read"))};
inline constexpr ts::Model wrappedModel{
    borrowFields, ts::group("services", ts::service<&wrappedService>("Read"))};
// The Service sits in the second group: its ID is 1:0 in every model.
inline constexpr auto readId = ts::idOf<ownModel, "services/Read">();
static_assert(readId == ts::idOf<wrappedModel, "services/Read">() && readId == 0x00010000u);
inline constexpr ::telemetry::descriptor::Descriptor ownDescriptor{ownModel};
inline constexpr ::telemetry::descriptor::Descriptor borrowDescriptor{borrowModel};
inline constexpr ::telemetry::descriptor::Descriptor wrappedDescriptor{wrappedModel};
inline std::array<std::byte, 65540> scratch{};
inline ts::Workspace owningWorkspace{std::span{scratch}.subspan(1)};
inline ts::Workspace emptyWorkspace{std::span<std::byte>{}};
inline constexpr rs::ValuesFile ownValues{ownDescriptor, owningWorkspace};
inline constexpr rs::ValuesFile borrowValues{borrowDescriptor, emptyWorkspace};
inline constexpr rs::ValuesFile wrappedValues{wrappedDescriptor, emptyWorkspace};
inline constexpr std::size_t valuesSize = 24 + 6 + 4097 + 65537 + 6;
inline std::array<std::byte, valuesSize> ownBytes{}, borrowBytes{}, expected{};

template<class U>
void le(std::byte* out, U value) noexcept
{
	for (std::size_t i = 0; i < sizeof(U); ++i)
		out[i] = static_cast<std::byte>((value >> (i * 8)) & 255u);
}

void oracle() noexcept
{
	expected.fill(std::byte{0});
	expected[0] = std::byte{'T'};
	expected[1] = std::byte{'V'};
	expected[2] = std::byte{'L'};
	expected[3] = std::byte{'3'};
	le(expected.data() + 4, std::uint16_t{3});
	le(expected.data() + 8, std::uint32_t{4});
	le(expected.data() + 12, std::uint32_t{valuesSize});
	le(expected.data() + 16, ownDescriptor.fingerprint());
	le(expected.data() + 25, config.value.value);
	expected[29] = std::byte{1};
	for (std::size_t i = 0; i < 4096; ++i)
		expected[31 + i] = static_cast<std::byte>((i * 37u + 17u) & 255u);
	for (std::size_t i = 0; i < 65536; ++i)
		expected[4128 + i] = static_cast<std::byte>((i * 37u + 23u) & 255u);
	expected[valuesSize - 6] = std::byte{1}; // Unavailable status plus five zero payload bytes.
}
} // namespace wire_fixture

int main(int argc, char** argv)
{
	using namespace wire_fixture;
	borrowed_test::start();
	fill(kib4.value, 17);
	fill(kib64.value, 23);
	check(ownModel.typeId<Config>() == borrowModel.typeId<Config>() &&
	      ownModel.typeId<Blob<4096>>() == borrowModel.typeId<Blob<4096>>() &&
	      ownModel.typeId<Blob<65536>>() == borrowModel.typeId<Blob<65536>>());
	check(ownModel.view().serviceTypeIds(readId)->requestTypeId ==
	          borrowModel.view().serviceTypeIds(readId)->requestTypeId &&
	      ownModel.view().serviceTypeIds(readId)->responseTypeId ==
	          borrowModel.view().serviceTypeIds(readId)->responseTypeId &&
	      ownModel.view().serviceTypeIds(readId)->requestTypeId ==
	          wrappedModel.view().serviceTypeIds(readId)->requestTypeId &&
	      ownModel.view().serviceTypeIds(readId)->responseTypeId ==
	          wrappedModel.view().serviceTypeIds(readId)->responseTypeId);
	check(ownDescriptor.valid() && borrowDescriptor.valid() &&
	      ownDescriptor.size() == borrowDescriptor.size() &&
	      ownDescriptor.fingerprint() == borrowDescriptor.fingerprint() &&
	      wrappedDescriptor.valid() && ownDescriptor.size() == wrappedDescriptor.size() &&
	      ownDescriptor.fingerprint() == wrappedDescriptor.fingerprint());
	std::array<std::byte, ownDescriptor.size()> ownDescription{}, borrowedDescription{},
	    wrappedDescription{};
	const auto descriptorA = ownDescriptor.read(0, ownDescription);
	const auto descriptorB = borrowDescriptor.read(0, borrowedDescription);
	const auto wrapped = wrappedDescriptor.read(0, wrappedDescription);
	check(descriptorA.status == ::telemetry::descriptor::ReadStatus::Ok &&
	      descriptorB.status == ::telemetry::descriptor::ReadStatus::Ok &&
	      wrapped.status == ::telemetry::descriptor::ReadStatus::Ok && descriptorA.eof &&
	      descriptorB.eof && wrapped.eof && ownDescription == borrowedDescription &&
	      ownDescription == wrappedDescription);
	for (unsigned i = 0; i < 4; ++i)
		check(ownModel.view().fieldTypeId(i) == borrowModel.view().fieldTypeId(i));
	check(borrowValues.requiredWorkspace() == 0 && borrowModel.maxFieldScratch() == 65536 &&
	      ownValues.requiredWorkspace() == 65536);
	check(ownValues.size() == valuesSize && borrowValues.size() == valuesSize &&
	      borrowValues.fieldCount() == 4 && borrowValues.maxTokenSize() == 65537);
	oracle();
	config.reads = kib4.reads = kib64.reads = 0;
	auto a = ownValues.read(0, ownBytes);
	check(a.status == resource::Status::Ok && a.eof && a.written == valuesSize &&
	      ownBytes == expected && owningWorkspace.used() == 0);
	auto b = borrowValues.read(0, borrowBytes);
	check(b.status == resource::Status::Ok && b.eof && b.written == valuesSize &&
	      borrowBytes == expected && emptyWorkspace.used() == 0);
	check(config.reads == 2 && kib4.reads == 2 && kib64.reads == 2 && ownBytes == borrowBytes);
	b = wrappedValues.read(0, borrowBytes);
	check(b.status == resource::Status::Ok && b.eof && b.written == valuesSize &&
	      borrowBytes == expected && emptyWorkspace.used() == 0);
	config.reads = kib4.reads = kib64.reads = 0;
	std::array<std::byte, 24> header{};
	b = borrowValues.read(0, header);
	check(b.status == resource::Status::Ok && b.next == 24 && b.written == 24 && !b.eof &&
	      config.reads == 0 && kib4.reads == 0 && kib64.reads == 0);
	std::array<std::byte, 5> tooShort{};
	b = borrowValues.read(24, tooShort);
	check(b.status == resource::Status::BufferTooSmall && b.written == 0 && config.reads == 0);
	std::array<std::byte, 6> smallToken{};
	b = borrowValues.read(24, smallToken);
	check(b.status == resource::Status::Ok && b.written == 6 && b.next == 30 && config.reads == 1 &&
	      std::equal(smallToken.begin(), smallToken.end(), expected.begin() + 24));
	b = borrowValues.read(25, smallToken);
	check(b.status == resource::Status::InvalidCursor && b.written == 0 && config.reads == 1);
	// A completed prefix never invokes a getter for the next oversized token.
	b = borrowValues.read(24, std::span{borrowBytes}.first(6 + 4096));
	check(b.status == resource::Status::Ok && b.written == 6 && b.next == 30 && config.reads == 2 &&
	      kib4.reads == 0);
	b = borrowValues.read(30, std::span{borrowBytes}.first(4097));
	check(b.status == resource::Status::Ok && b.written == 4097 && b.next == 4127 &&
	      kib4.reads == 1 && config.reads == 2);
	b = borrowValues.read(4127, std::span{borrowBytes}.first(65537));
	check(b.status == resource::Status::Ok && b.written == 65537 && b.next == valuesSize - 6 &&
	      kib64.reads == 1 && pattern<65536>(std::span{borrowBytes}.subspan(1, 65536), 23));
	b = borrowValues.read(valuesSize - 6, smallToken);
	check(b.status == resource::Status::Ok && b.eof && smallToken[0] == std::byte{1} &&
	      std::all_of(smallToken.begin() + 1, smallToken.end(), [](std::byte value) {
		      return value == std::byte{0};
	      }));
	// The compiled Model adapter reaches the same borrowed thunks.
	std::array<std::byte, 5> adapterOutput{};
	const auto field = ts::readFieldEncoded(borrowModel.view(), 0u, adapterOutput, emptyWorkspace);
	check(field.dispatch == DS::Ok && field.written == 5 &&
	      configBytes(adapterOutput, config.value));
	const std::array<std::byte, 2> input{std::byte{1}, std::byte{7}};
	std::array<std::byte, 64> requestScratch{};
	ts::Workspace serviceWorkspace{requestScratch};
	const auto service =
	    ts::callServiceEncoded(borrowModel.view(), readId, input, adapterOutput, serviceWorkspace);
	check(service.dispatch == DS::Ok && service.endpointStatus == SS::Ok && service.written == 5 &&
	      configBytes(adapterOutput, config.value) && serviceWorkspace.used() == 0);
	const auto wrappedService =
	    ts::callServiceEncoded(wrappedModel.view(), readId, input, adapterOutput, serviceWorkspace);
	check(wrappedService.dispatch == DS::Ok && wrappedService.endpointStatus == SS::Ok &&
	      wrappedService.written == 5 && configBytes(adapterOutput, config.value) &&
	      serviceWorkspace.used() == 0);
#ifndef BORROWED_ARM
	// Keep emitted bytes in the caller-selected artifact directory. File I/O
	// is outside the core-operation allocation observation.
	borrowed_test::watching = false;
	bool saved = false;
	if (argc == 2) {
		if (auto* file = std::fopen(argv[1], "wb")) {
			saved = std::fwrite(ownDescription.data(), 1, ownDescription.size(), file) ==
			            ownDescription.size() &&
			        std::fwrite(expected.data(), 1, expected.size(), file) == expected.size();
			saved = std::fclose(file) == 0 && saved;
		}
	}
	check(saved);
#else
	(void)argc;
	(void)argv;
#endif
	return borrowed_test::finish(); // 27 on host (including artifact and allocation checks).
}

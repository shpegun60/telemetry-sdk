/*
 * @file DescriptorCheck.cpp
 * @brief Every-offset/chunk streaming, identity, names and construction checks.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "Fixture.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

namespace df = descriptor_fixture;
namespace ts = telemetry;
std::uint64_t descriptorOtherFingerprint() noexcept;
const std::byte* descriptorOtherBytes() noexcept;

template<class Descriptor, std::size_t N>
void checkReads(const Descriptor& descriptor, const std::array<std::byte, N>& expected)
{
	std::array<std::byte, 257> out;
	for (std::size_t offset = 0; offset <= N; ++offset) {
		for (std::size_t chunk : {0u, 1u, 2u, 3u, 7u, 8u, 15u, 16u, 31u, 64u, 257u}) {
			out.fill(std::byte{0xa5});
			const auto r = descriptor.read(offset, std::span{out}.first(chunk));
			const auto count = std::min(chunk, N - offset);
			assert(r.status == (chunk == 0 && offset != N
			                        ? ::telemetry::descriptor::ReadStatus::BufferTooSmall
			                        : ::telemetry::descriptor::ReadStatus::Ok));
			assert(r.written == count && r.next == offset + count &&
			       r.eof == (offset + count == N));
			assert(std::equal(out.begin(), out.begin() + count, expected.begin() + offset));
			assert(std::all_of(out.begin() + count, out.end(), [](auto b) {
				return b == std::byte{0xa5};
			}));
		}
	}
	for (std::size_t chunk = 1; chunk <= 129; ++chunk) {
		std::array<std::byte, N> complete{};
		::telemetry::descriptor::Cursor cursor = 0;
		while (cursor != N) {
			const auto r = descriptor.read(
			    cursor, std::span{complete}.subspan(cursor, std::min(chunk, N - cursor)));
			assert(r.status == ::telemetry::descriptor::ReadStatus::Ok && r.next > cursor);
			cursor = r.next;
		}
		assert(complete == expected);
	}
	for (const auto bad :
	     {::telemetry::descriptor::Cursor{N + 1}, ::telemetry::descriptor::Cursor{1} << 32,
	      std::numeric_limits<::telemetry::descriptor::Cursor>::max()}) {
		out.fill(std::byte{0xa5});
		auto r = descriptor.read(bad, out);
		assert(r.status == ::telemetry::descriptor::ReadStatus::InvalidCursor && r.next == bad &&
		       r.written == 0 && !r.eof);
		assert(out[0] == std::byte{0xa5});
	}
}

template<class Descriptor>
auto readAll(const Descriptor& d)
{
	std::array<std::byte, df::edge.size() + 128> bytes{};
	auto r = d.read(0, bytes);
	assert(r.status == ::telemetry::descriptor::ReadStatus::Ok && r.eof && r.written == d.size());
	return bytes;
}

int main(int argc, char** argv)
{
	assert(descriptorOtherFingerprint() == df::edge.fingerprint());
	assert(descriptorOtherBytes() == df::edgeBytes.data());
	checkReads(df::mixed, df::mixedBytes);
	checkReads(df::edge, df::edgeBytes);
	checkReads(df::empty, df::emptyBytes);
	assert(fixture::device.reads == 0 && fixture::device.commands == 0 &&
	       fixture::device.services == 0 && df::calls == 0);

	// Reconstruct with other owners. Every callback address/value differs,
	// while descriptor bytes remain identical to the constexpr instance.
	fixture::Device other;
	other.current.target = 999;
	const ts::FieldTable otherFields{
	    ts::field<&fixture::Device::config, &fixture::Device::setConfig>("Config", other)};
	const ts::FieldTable originalFields{
	    ts::field<&fixture::Device::config, &fixture::Device::setConfig>("Config",
	                                                                     fixture::device)};
	const ts::testing::FieldCatalogTable a{ts::testing::group("motor", otherFields)};
	const ts::testing::FieldCatalogTable b{ts::testing::group("motor", originalFields)};
	const ts::testing::V30Model ma{a, ts::testing::emptyCommands, df::noServices};
	const ts::testing::V30Model mb{b, ts::testing::emptyCommands, df::noServices};
	const ::telemetry::descriptor::Descriptor da{ma}, db{mb};
	assert(da.fingerprint() == db.fingerprint() && readAll(da) == readAll(db));
	assert(other.reads == 0);

	auto before = readAll(df::edge);
	df::getter.bind(&df::read<std::uint32_t>);
	df::setter.bind(+[](std::uint32_t) noexcept {
		++df::calls;
		return telemetry::WriteResult::Applied;
	});
	const auto rebound = ::telemetry::descriptor::withoutTypeNames(df::model);
	assert(rebound.fingerprint() == df::edge.fingerprint() && readAll(rebound) == before);
	df::getter.reset();
	df::setter.reset();
	assert(df::calls == 0);

	const ts::FieldTable ro{ts::field<&fixture::Device::config>("Config", fixture::device)};
	const ts::testing::FieldCatalogTable rc{ts::testing::group("motor", ro)};
	const ts::testing::V30Model rm{rc, ts::testing::emptyCommands, df::noServices};
	const ::telemetry::descriptor::Descriptor rd{rm};
	assert(rd.fingerprint() != da.fingerprint());

	// Duplicate names are scoped to category/catalog. Runtime construction
	// returns an invalid descriptor without publishing partially built data.
	const ts::testing::FieldCatalogTable duplicate{ts::testing::group("same", ro),
	                                               ts::testing::group("same", otherFields)};
	const ts::testing::V30Model duplicateModel{duplicate, ts::testing::emptyCommands,
	                                           df::noServices};
	const ::telemetry::descriptor::Descriptor invalid{duplicateModel};
	assert(invalid.error() == ::telemetry::descriptor::DescriptorError::DuplicateName &&
	       invalid.size() == 0 && invalid.fingerprint() == 0);
	std::array<std::byte, 8> scratch{};
	const auto failure = invalid.read(17, scratch);
	assert(failure.status == ::telemetry::descriptor::ReadStatus::InvalidData &&
	       failure.next == 17 && failure.written == 0);

	// Mutable C-string buffers may have spare capacity; names are borrowed
	// and remain immutable throughout each descriptor's lifetime.
	char label[32] = "renamed";
	const ts::FieldTable renamed{ts::field<&fixture::Device::config>(label, fixture::device)};
	const ts::testing::FieldCatalogTable renamedCatalogs{ts::testing::group("motor", renamed)};
	const ts::testing::V30Model renamedModel{renamedCatalogs, ts::testing::emptyCommands,
	                                         df::noServices};
	const ::telemetry::descriptor::Descriptor renamedDescriptor{renamedModel};
	assert(renamedDescriptor.fingerprint() != rd.fingerprint());

	// Resume inside a maximal-length label without scanning its prefix.
	std::array<char, ts::Limits::maxStringBytes + 1> longName{};
	std::fill(longName.begin(), longName.end() - 1, 'x');
	const ts::FieldTable longRow{ts::field<&df::read<std::uint32_t>>(longName.data())};
	const ts::testing::FieldCatalogTable longCatalog{ts::testing::group("long", longRow)};
	const ts::testing::V30Model longModel{longCatalog, ts::testing::emptyCommands, df::noServices};
	const ::telemetry::descriptor::Descriptor longDescriptor{longModel};
	for (auto cursor = longDescriptor.size() - ts::Limits::maxStringBytes;
	     cursor < longDescriptor.size(); ++cursor) {
		auto r = longDescriptor.read(cursor, std::span{scratch}.first(1));
		assert(r.status == ::telemetry::descriptor::ReadStatus::Ok && r.written == 1 &&
		       scratch[0] == std::byte{'x'});
	}
	assert(df::calls == 0);

	if (argc == 4) {
		const auto save = [](const char* path, const auto& bytes) {
			auto* file = std::fopen(path, "wb");
			assert(file != nullptr);
			assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
			assert(std::fclose(file) == 0);
		};
		save(argv[1], df::mixedBytes);
		save(argv[2], df::edgeBytes);
		save(argv[3], df::emptyBytes);
	}
	std::puts("Descriptor streaming, fingerprint and lifetime checks passed");
}

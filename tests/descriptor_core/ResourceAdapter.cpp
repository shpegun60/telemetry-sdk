/*
 * @file ResourceAdapter.cpp
 * @brief Canonical byte slices and the actual Resource adapter status mapping.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "../structured/descriptor/Fixture.hpp"
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <type_traits>

namespace td = telemetry::descriptor;
namespace rs = resource::telemetry::v3;
namespace df = descriptor_fixture;
static_assert(sizeof(td::detail::Segment) == 12);
static_assert(std::is_same_v<decltype(df::mixed.read(0, {})), td::ReadResult>);
static_assert(
    std::is_same_v<decltype(rs::DescriptorFile{df::mixed}.read(0, {})), resource::ReadResult>);

template<class Core, std::size_t N>
std::size_t compare(const Core& core, const std::array<std::byte, N>& packed)
{
	assert(core.size() == packed.size());
	std::array<std::byte, 67> a{}, b{}, c{};
	const auto view = core.view();
	const rs::DescriptorFile coreFile{core};
	const rs::DescriptorFile packedFile{packed};
	std::size_t checks = 0;
	for (std::uint64_t offset = 0; offset <= N + 1; ++offset) {
		for (std::size_t chunk : {0u, 1u, 2u, 7u, 16u, 67u}) {
			a.fill(std::byte{0xa5});
			b = c = a;
			const auto x = view.read(offset, std::span{a}.first(chunk));
			const auto y = coreFile.read(offset, std::span{b}.first(chunk));
			const auto z = packedFile.read(offset, std::span{c}.first(chunk));
			const auto expectedCore = offset > N    ? td::ReadStatus::InvalidCursor
			                          : offset == N ? td::ReadStatus::Ok
			                          : chunk == 0  ? td::ReadStatus::BufferTooSmall
			                                        : td::ReadStatus::Ok;
			const auto expectedResource = offset > N    ? resource::Status::InvalidCursor
			                              : offset == N ? resource::Status::Ok
			                              : chunk == 0  ? resource::Status::BufferTooSmall
			                                            : resource::Status::Ok;
			const auto count = offset <= N ? std::min<std::size_t>(chunk, N - offset) : 0;
			assert(x.status == expectedCore && y.status == expectedResource &&
			       z.status == expectedResource);
			assert(x.next == offset + count && x.written == count &&
			       x.eof == (offset + count == N));
			assert(y.next == x.next && y.written == x.written && y.eof == x.eof);
			assert(z.next == x.next && z.written == x.written && z.eof == x.eof);
			assert(a == b && b == c);
			if (count != 0) {
				assert(std::equal(a.begin(), a.begin() + count, packed.begin() + offset));
			}
			assert(std::all_of(a.begin() + count, a.end(), [](auto value) {
				return value == std::byte{0xa5};
			}));
			++checks;
		}
	}
	return checks;
}

int main(int argc, char** argv)
{
	const auto checks = compare(df::mixed, df::mixedBytes) + compare(df::edge, df::edgeBytes) +
	                    compare(df::empty, df::emptyBytes);
	const telemetry::FieldTable fields{telemetry::field<&df::read<std::uint32_t>>("value")};
	const telemetry::testing::FieldCatalogTable groups{telemetry::testing::group("same", fields),
	                                                   telemetry::testing::group("same", fields)};
	const telemetry::testing::V30Model model{groups, telemetry::testing::emptyCommands,
	                                         df::noServices};
	const td::Descriptor invalid{model};
	assert(invalid.error() == td::DescriptorError::DuplicateName && invalid.size() == 0 &&
	       invalid.fingerprint() == 0);
	for (const auto cursor : {td::Cursor{0}, std::numeric_limits<td::Cursor>::max()}) {
		// Definition refusal precedes invalid cursor and empty-buffer checks.
		assert(invalid.read(cursor, {}).status == td::ReadStatus::InvalidData);
		const auto result = rs::DescriptorFile{invalid}.read(cursor, {});
		assert(result.status == resource::Status::InvalidData && result.next == cursor &&
		       result.written == 0 && !result.eof);
	}
	assert(fixture::device.reads == 0 && fixture::device.commands == 0 &&
	       fixture::device.services == 0 && df::calls == 0);
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
	std::printf("Descriptor core/Resource: %zu equivalent slice checks passed\n", checks);
}

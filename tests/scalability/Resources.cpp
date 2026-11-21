/*
 * @file Resources.cpp
 * @brief Streaming/packed descriptor and whole-token values scaling.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 *
 * Use an actual typed FieldTable and Model. Repeated scalar types share one
 * registry entry. Host timings are not MCU cycle claims; fixture-owned output
 * allocation stays outside timed reads. The runtime descriptor/value indexes
 * have the same size as constexpr instances but reside in host local storage.
 */
#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace ts = telemetry;
namespace rv = resource::telemetry::v3;

// Retains one scalar and counts its sampling independently of metadata reads.
// Public methods: read(): Sample current value.
struct Owner {
	std::uint32_t value = 0;
	mutable unsigned calls = 0;

	std::uint32_t read() const noexcept
	{
		++calls;
		return value;
	}
};

inline std::array<Owner, ROWS> owners;
inline constexpr auto labels = [] {
	std::array<std::array<char, 7>, ROWS> result{};
	for (unsigned i = 0; i < ROWS; ++i) {
		result[i][0] = 'v';
		for (unsigned digit = 0, value = i; digit < 5; ++digit, value /= 10)
			result[i][5 - digit] = char('0' + value % 10);
	}
	return result;
}();

template<std::size_t... I>
constexpr auto makeTable(std::index_sequence<I...>)
{
	return ts::FieldTable{ts::field<&Owner::read>(labels[I].data(), owners[I])...};
}

inline constexpr auto table = makeTable(std::make_index_sequence<ROWS>{});
inline constexpr ts::testing::FieldCatalogTable fields{ts::testing::group("scale", table)};
inline constexpr ts::testing::ServiceCatalogTable services{};
inline constexpr ts::testing::V30Model model{fields, ts::testing::emptyCommands, services};
inline constexpr ::telemetry::descriptor::Descriptor staticDescriptor{model};
inline constexpr auto packedBytes = ::telemetry::descriptor::packDescriptor<staticDescriptor>();
using Descriptor = std::remove_cv_t<decltype(staticDescriptor)>;
using Model = std::remove_cv_t<decltype(model)>;
inline ts::Workspace workspace{std::span<std::byte>{}};

unsigned checks = 0;

void require(bool condition)
{
	++checks;
	if (!condition)
		std::abort();
}

[[gnu::noinline]] Descriptor* construct(void* storage, const decltype(model)* source) noexcept
{
	return std::construct_at(static_cast<Descriptor*>(storage), *source);
}

// Check actual partial READ bytes independently of the timing loop's lengths.
bool verifyChunks(const Descriptor& file, std::span<const std::byte> expected, unsigned capacity)
{
	std::vector<std::byte> output(expected.size(), std::byte{0xff});
	std::size_t used = 0;
	while (used < output.size()) {
		const auto room = std::min<std::size_t>(capacity, output.size() - used);
		const auto result = file.read(used, std::span{output}.subspan(used, room));
		if (result.status != decltype(result.status)::Ok || result.written == 0 ||
		    result.written > room || result.next != used + result.written ||
		    result.eof != (result.next == output.size()))
			return false;
		used += result.written;
	}
	return std::equal(output.begin(), output.end(), expected.begin());
}

template<class File>
std::uint64_t consume(const File& file, unsigned capacity)
{
	std::array<std::byte, 512> buffer;
	resource::Cursor cursor = 0;
	std::uint64_t bytes = 0;
	while (cursor < file.size()) {
		const auto result = file.read(cursor, std::span{buffer}.first(capacity));
		if (result.status != decltype(result.status)::Ok || result.next <= cursor)
			return 0;
		// A compiler memory input keeps produced bytes materialized even when
		// the caller only accumulates lengths. This emits no checksum loop and
		// prevents an unused packed memcpy from disappearing from the timing.
		asm volatile("" : : "m"(buffer) : "memory");
		bytes += result.written;
		cursor = result.next;
	}
	return bytes;
}

template<class File>
double timed(const File& file, unsigned capacity)
{
	std::array<double, 9> samples;
	for (auto& sample : samples) {
		constexpr unsigned repeats = 20;
		std::uint64_t bytes = 0;
		const auto start = std::chrono::steady_clock::now();
		for (unsigned repeat = 0; repeat < repeats; ++repeat)
			bytes += consume(file, capacity);
		sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
		             .count() /
		         repeats;
		require(bytes == std::uint64_t{file.size()} * repeats);
	}
	std::sort(samples.begin(), samples.end());
	return samples[4];
}

int main()
{
	for (unsigned i = 0; i < ROWS; ++i)
		owners[i].value = 17 + i;
	alignas(Descriptor) std::array<std::byte, sizeof(Descriptor)> storage;
	Descriptor* (*volatile build)(void*, const decltype(model)*) noexcept = construct;
	std::array<double, 9> construction;
	for (auto& sample : construction) {
		const auto start = std::chrono::steady_clock::now();
		auto* descriptor = build(storage.data(), &model);
		sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
		             .count();
		require(descriptor->valid() && descriptor->fingerprint() == staticDescriptor.fingerprint());
		std::destroy_at(descriptor);
	}
	std::sort(construction.begin(), construction.end());
	const auto* descriptor = build(storage.data(), &model);
	rv::ValuesFile values{*descriptor, workspace};
	require(values.size() == 24 + ROWS * 5 && values.requiredWorkspace() == 0);
	require(values.maxTokenSize() == 5 && model.maxScratch() == 0);
	std::vector<std::byte> bytes(descriptor->size());
	const auto all = descriptor->read(0, bytes);
	require(all.status == ::telemetry::descriptor::ReadStatus::Ok && all.eof &&
	        bytes.size() == packedBytes.size());
	require(std::equal(bytes.begin(), bytes.end(), packedBytes.begin()));
	require(verifyChunks(*descriptor, packedBytes, 128));
	require(verifyChunks(*descriptor, packedBytes, 512));
	for (const auto& owner : owners)
		require(owner.calls == 0); // Construction and metadata reads never sample values.
	std::array<std::byte, 5> token;
	const auto middle = ROWS / 2;
	const auto cursor = 24 + middle * 5;
	const auto shortRead = values.read(cursor, std::span{token}.first(4));
	require(shortRead.status == resource::Status::BufferTooSmall && owners[middle].calls == 0);
	const auto valueRead = values.read(cursor, token);
	require(valueRead.status == resource::Status::Ok && valueRead.next == cursor + 5);
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t(std::to_integer<unsigned>(token[i + 1])) << (i * 8);
	require(token[0] == std::byte{0} && value == 17 + middle);
	for (unsigned i = 0; i < ROWS; ++i)
		require(owners[i].calls == (i == middle ? 1u : 0u));
	require(values.read(cursor + 1, token).status == resource::Status::InvalidCursor);
	require(owners[middle].calls == 1);
	for (auto& owner : owners)
		owner.calls = 0;
	require(consume(values, 128) == values.size());
	for (const auto& owner : owners)
		require(owner.calls == 1); // A complete stream samples each row exactly once.
	std::vector<std::byte> valueBytes(values.size());
	const auto allValues = values.read(0, valueBytes);
	require(allValues.status == resource::Status::Ok && allValues.eof &&
	        allValues.written == valueBytes.size() && allValues.next == valueBytes.size());
	for (unsigned i = 0; i < ROWS; ++i) {
		const auto first = 24 + i * 5;
		std::uint32_t decoded = 0;
		for (unsigned byte = 0; byte < 4; ++byte)
			decoded |= std::uint32_t(std::to_integer<unsigned>(valueBytes[first + 1 + byte]))
			           << (byte * 8);
		require(valueBytes[first] == std::byte{0} && decoded == 17 + i);
	}
	rv::DescriptorFile packed{packedBytes};
	const auto streaming128 = timed(*descriptor, 128);
	const auto streaming512 = timed(*descriptor, 512);
	const auto packed128 = timed(packed, 128);
	const auto values128 = timed(values, 128);
	require(workspace.used() == 0);
	std::printf("{\"rows\":%u,\"checks\":%u,\"type_count\":%u,\"table_bytes\":%zu,"
	            "\"descriptor_object_bytes\":%zu,\"descriptor_index_bytes\":%zu,"
	            "\"descriptor_wire_bytes\":%u,\"values_object_bytes\":%zu,"
	            "\"values_wire_bytes\":%u,\"workspace_bytes\":%u,"
	            "\"construct_median_us\":%.3f,\"stream128_median_us\":%.3f,"
	            "\"stream512_median_us\":%.3f,\"packed128_median_us\":%.3f,"
	            "\"values128_median_us\":%.3f}\n",
	            ROWS, checks, Model::Registry::typeCount, sizeof(table), sizeof(Descriptor),
	            Descriptor::indexBytes, descriptor->size(), sizeof(values), values.size(),
	            values.requiredWorkspace(), construction[4], streaming128, streaming512, packed128,
	            values128);
	std::destroy_at(descriptor);
}

/*
 * @file Fixture.hpp
 * @brief Stable native fixtures and a padded application Service for mapping measurements.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_EXAMPLES_READ_MANY_MAPPING_FIXTURE_HPP
#define TELEMETRY_EXAMPLES_READ_MANY_MAPPING_FIXTURE_HPP
#pragma once

#include "Prototype.hpp"
#include "../read_comparison/DataModel.hpp"
#include <telemetry/descriptor/Descriptor.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace read_many_mapping::fixture {
namespace ts = telemetry;

inline constexpr std::array<const char*, 16> names{"F00", "F01", "F02", "F03", "F04", "F05",
                                                   "F06", "F07", "F08", "F09", "F10", "F11",
                                                   "F12", "F13", "F14", "F15"};

struct ScalarOwner {
	std::array<std::uint64_t, 32> calls{};
	std::uint32_t workIterations = 0, workChecksum = 0;

	template<std::size_t I>
	std::uint32_t read() noexcept
	{
		++calls[I];
		auto work = workChecksum ^ (0x9e3779b9u + std::uint32_t(I));
		for (std::uint32_t j = 0; j < workIterations; ++j)
			work = (work ^ (work >> 13)) * 1664525u + 1013904223u;
		workChecksum = work;
		return read_comparison::data[I];
	}

	void reset(std::uint32_t work = 0) noexcept
	{
		calls.fill(0);
		workIterations = work;
		workChecksum = 0;
	}

	std::uint64_t total() const noexcept
	{
		std::uint64_t result = 0;
		for (auto count : calls)
			result += count;
		return result;
	}
};

inline ScalarOwner scalarOwner;

template<std::size_t Offset, std::size_t... I>
constexpr auto scalarFields(std::index_sequence<I...>) noexcept
{
	return ts::FieldTable{
	    ts::field<&ScalarOwner::template read<Offset + I>>(names[I], scalarOwner)...};
}

inline constexpr auto left = scalarFields<0>(std::make_index_sequence<16>{});
inline constexpr auto right = scalarFields<16>(std::make_index_sequence<16>{});

// A group of sixteen scalar Fields, and any further endpoints of the group.
template<std::size_t Offset, std::size_t... I, class... Extra>
constexpr auto scalarGroup(const char* name, std::index_sequence<I...>,
                           const Extra&... extra) noexcept
{
	return ts::group(name,
	                 ts::field<&ScalarOwner::template read<Offset + I>>(names[I], scalarOwner)...,
	                 extra...);
}

inline constexpr ts::Model scalarModel{scalarGroup<0>("left", std::make_index_sequence<16>{}),
                                       scalarGroup<16>("right", std::make_index_sequence<16>{})};
inline constexpr ts::descriptor::Descriptor scalarDescriptor{scalarModel};
inline constexpr auto scalarDescriptorBytes = ts::descriptor::packDescriptor<scalarDescriptor>();

// Preserve every declared endpoint/type while adapting one erased read boundary.
// These controls cover conservative scratch and a thunk which fails after writing.
struct AdaptedModel {
	std::array<ts::FieldEntry, 16> rows{};
	std::array<ts::FieldCatalog, 2> catalogs{};
	ts::ModelView view = scalarModel.view();

	AdaptedModel(ts::FieldEntry::Read read, std::uint32_t scratch) noexcept
	{
		std::copy_n(left.data(), rows.size(), rows.begin());
		rows[0].readContext = left.data();
		rows[0].read = read;
		rows[0].readScratchBytes = scratch;
		catalogs = {ts::FieldCatalog{"left", rows.data(), 16},
		            ts::FieldCatalog{"right", right.data(), 16}};
		view.fields = ts::FieldIndex{catalogs.data(), 2};
	}

	AdaptedModel(const AdaptedModel&) = delete;
	AdaptedModel& operator=(const AdaptedModel&) = delete;

	static ts::EncodedReadResult scratchRead(const void* context, std::byte* output,
	                                         ts::Workspace& workspace) noexcept
	{
		ts::Workspace::Lease<std::array<std::byte, 16>> temporary{workspace};
		if (!temporary.valid())
			return {ts::DispatchStatus::WorkspaceTooSmall, 0};
		return static_cast<const ts::FieldEntry*>(context)->readEncoded({output, 4}, workspace);
	}

	static ts::EncodedReadResult partialRead(const void* context, std::byte* output,
	                                         ts::Workspace& workspace) noexcept
	{
		const auto read =
		    static_cast<const ts::FieldEntry*>(context)->readEncoded({output, 4}, workspace);
		if (read.dispatch != ts::DispatchStatus::Ok)
			return read;
		output[0] = std::byte{0xa5};
		return {ts::DispatchStatus::InternalError, 4};
	}
};

constexpr std::uint32_t packedId(std::size_t linear) noexcept
{
	return (std::uint32_t(linear / 16) << 16) | std::uint32_t(linear % 16);
}

constexpr std::size_t linearId(std::uint32_t id) noexcept
{
	return (id >> 16) * 16 + (id & 0xffffu);
}

inline std::array<std::uint32_t, maxItems> selection(std::size_t count) noexcept
{
	std::array<std::uint32_t, maxItems> result{};
	if (count != 0 && count <= maxItems)
		for (std::size_t i = 0; i < count; ++i)
			result[i] = packedId(i * 32 / count);
	return result;
}

enum class Mode : std::int16_t {
	Reverse = -7,
	Forward = 9
};

struct OneWord {
	std::uint32_t value;
};

struct MixedOwner {
	std::uint64_t calls = 0;
	std::uint32_t f32bits = 0x7fc12345u;
	std::uint64_t f64bits = 0xfff0123456789abcULL;

	bool boolean() noexcept
	{
		++calls;
		return true;
	}

	std::uint64_t wide() noexcept
	{
		++calls;
		return UINT64_MAX;
	}

	std::int64_t signedWide() noexcept
	{
		++calls;
		return INT64_MIN;
	}

	float real32() noexcept
	{
		++calls;
		return std::bit_cast<float>(f32bits);
	}

	double real64() noexcept
	{
		++calls;
		return std::bit_cast<double>(f64bits);
	}

	Mode mode() noexcept
	{
		++calls;
		return static_cast<Mode>(-123);
	}

	OneWord record() noexcept
	{
		++calls;
		return {17};
	}

	std::uint32_t missing() noexcept
	{
		++calls;
		return 99;
	}
};

inline MixedOwner mixedOwner;
inline ts::OwnerSlot<MixedOwner> emptyOwner;

template<class... Extra>
constexpr auto mixedLeft(const Extra&... extra) noexcept
{
	return ts::group("left", ts::field<&MixedOwner::boolean>("Bool", mixedOwner),
	                 ts::field<&MixedOwner::wide>("U64", mixedOwner),
	                 ts::field<&MixedOwner::signedWide>("S64", mixedOwner),
	                 ts::field<&MixedOwner::real32>("F32", mixedOwner), extra...);
}

inline constexpr auto mixedRight =
    ts::group("right", ts::field<&MixedOwner::real64>("F64", mixedOwner),
              ts::field<&MixedOwner::mode>("Enum", mixedOwner),
              ts::field<&MixedOwner::record>("Struct4", mixedOwner),
              ts::field<&MixedOwner::missing>("Unavailable", emptyOwner));
inline constexpr ts::Model mixedModel{mixedLeft(), mixedRight};
inline constexpr ts::descriptor::Descriptor mixedDescriptor{mixedModel};
inline constexpr auto mixedDescriptorBytes = ts::descriptor::packDescriptor<mixedDescriptor>();
inline constexpr std::array<std::uint32_t, 8> mixedIds{0,        1,        2,        3,
                                                       0x10000u, 0x10001u, 0x10002u, 0x10003u};

struct FixedRequest {
	std::uint16_t count;
	std::array<std::uint32_t, maxItems> ids;
};

struct FixedItem {
	std::uint32_t id;
	std::uint8_t status, length;
	std::uint16_t reserved;
	std::array<std::uint8_t, maxValueBytes> value;
};

struct FixedReply {
	std::uint16_t count;
	std::array<FixedItem, maxItems> items;
};

static_assert(ts::wireSize<FixedRequest> == 130 && ts::wireSize<FixedReply> == 514);

// The ordinary Service callback delegates to exactly the compact prototype's
// executor. Workspace and Batch are supplied by its synchronous application owner.
struct ServiceOwner {
	ts::ModelView fields = scalarModel.view();
	ts::Workspace* workspace = nullptr;
	Batch* batch = nullptr;
	GlobalStatus last = GlobalStatus::NotAdmitted;
	std::uint64_t calls = 0, explicitValueCopyBytes = 0, explicitIdCopyBytes = 0;

	ts::ServiceResult<FixedReply> read(const FixedRequest& request) noexcept
	{
		++calls;
		if (!workspace || !batch || request.count == 0 || request.count > maxItems) {
			last = GlobalStatus::Malformed;
			return ts::ServiceResult<FixedReply>::failure(ts::ServiceStatus::InvalidArgument);
		}
		// Service decoding borrows the outer Workspace. Keep active IDs outside
		// it before invoking the executor's disjoint-input scratch contract.
		std::array<std::uint32_t, maxItems> ids{};
		std::copy_n(request.ids.begin(), request.count, ids.begin());
		explicitIdCopyBytes += request.count * sizeof(std::uint32_t);
		last = execute(fields, std::span{ids}.first(request.count), *workspace, *batch);
		if (last != GlobalStatus::Ok)
			return ts::ServiceResult<FixedReply>::failure(ts::ServiceStatus::InvalidArgument);
		return ts::ServiceResult<FixedReply>::successFrom([&]() noexcept -> FixedReply {
			FixedReply reply{};
			reply.count = batch->count;
			for (std::size_t i = 0; i < batch->count; ++i) {
				const auto& source = batch->items[i];
				auto& item = reply.items[i];
				item.id = source.id;
				item.status = static_cast<std::uint8_t>(source.status);
				item.length = source.length;
				for (std::size_t j = 0; j < source.length; ++j)
					item.value[j] = std::to_integer<std::uint8_t>(source.value[j]);
				explicitValueCopyBytes += source.length;
			}
			return reply;
		});
	}
};

inline ServiceOwner serviceOwner;
// The selection Service joins group left: its ID is 0, the one a request carries
// by default, and the Fields keep their IDs.
inline constexpr auto readSelection =
    ts::service<&ServiceOwner::read>("ReadSelection", serviceOwner);
inline constexpr ts::Model serviceModel{
    scalarGroup<0>("left", std::make_index_sequence<16>{}, readSelection),
    scalarGroup<16>("right", std::make_index_sequence<16>{})};
inline constexpr ts::Model mixedServiceModel{mixedLeft(readSelection), mixedRight};
static_assert(ts::idOf<serviceModel, "left/ReadSelection">() == 0 &&
              ts::idOf<mixedServiceModel, "left/ReadSelection">() == 0);

// Parsing caller storage is deliberately shared by Host and no-heap qualification.
struct Metadata {
	std::array<telemetry::runtime::RuntimeType, 48> types{};
	std::array<telemetry::runtime::RuntimeMember, 8> members{};
	std::array<telemetry::runtime::RuntimeEnumEntry, 16> enums{};
	// A tree of two groups: one catalog per group in each of the three families.
	std::array<telemetry::runtime::RuntimeCatalog, 6> catalogs{};
	std::array<telemetry::runtime::RuntimeEndpoint, 32> endpoints{};
	telemetry::runtime::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs,
	                                                     endpoints};
	telemetry::runtime::RuntimeSchema schema;
	bool valid = false;

	explicit Metadata(Input descriptor) noexcept
	{
		const auto result = telemetry::runtime::RuntimeSchema::parse(descriptor, workspace);
		valid = bool(result);
		if (result)
			schema = result.schema;
	}
};
} // namespace read_many_mapping::fixture
#endif // TELEMETRY_EXAMPLES_READ_MANY_MAPPING_FIXTURE_HPP

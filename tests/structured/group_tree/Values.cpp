/*
 * @file Values.cpp
 * @brief The live Values file of a group-tree Model: layout, tokens and a host read-back.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * resource::telemetry::v3::ValuesFile over Model{adc::tlm::branch,
 * metrology::tlm::branch}: it compiles for a tree (it counted its Fields
 * through a catalog Model's own table before), lays out one token per Field
 * in descriptor order (groups in pre-order, a group's Fields in declaration
 * order), carries the tree descriptor's fingerprint, and reads back through
 * the host's ValuesReader against the RuntimeSchema of the same descriptor,
 * value for value with telemetry::read<model, path>().
 */

#include "AdcBranch.hpp"
#include "MetrologyBranch.hpp"

#include <integration/telemetry/v3/ValuesFile.hpp>
#include <integration/telemetry/v3/ValuesReader.hpp>
#include <telemetry/runtime/RuntimeSchema.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace values_fixture {
namespace ts = telemetry;
namespace rs = resource::telemetry::v3;

inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto packed = ::telemetry::descriptor::packDescriptor<descriptor>();
inline std::array<std::byte, model.maxFieldScratch()> storage;
inline ts::Workspace workspace{storage};
inline constexpr rs::ValuesFile values{descriptor, workspace};

// Five Fields: adc/Config, adc/Status, adc/channel1/Gain, adc/channel2/Gain,
// metrology/Config. A token is one status byte and the canonical value:
// Config 4+2+1, Status 2+2*2, Gain 4, metrology::Config 4+1.
static_assert(values.fieldCount() == 5);
static_assert(values.size() ==
              rs::valuesHeaderBytes + (1 + 7) + (1 + 6) + (1 + 4) + (1 + 4) + (1 + 5));
static_assert(values.maxTokenSize() == 8);
static_assert(values.fingerprint() == descriptor.fingerprint() && values.fingerprint() != 0);

// The same Fields as the catalog engine under the tree see them: one row per group.
static_assert(model.view().fieldCatalogCount == 4);

} // namespace values_fixture

namespace {
namespace vf = values_fixture;
namespace ts = telemetry;
namespace rs = resource::telemetry::v3;

int failures = 0;
int checks = 0;

void check(bool condition, const char* what)
{
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAIL %s\n", what);
	}
}

template<class T>
T decoded(std::span<const std::byte> payload) noexcept
{
	T value{};
	std::memcpy(&value, payload.data(), sizeof value);
	return value;
}

} // namespace

int main()
{
	// The whole file in one read, and again in pieces of every length up to 13.
	std::array<std::byte, vf::values.size()> whole{};
	const auto read = vf::values.read(0, whole);
	check(read.status == resource::Status::Ok && read.written == whole.size() && read.eof,
	      "read the whole Values file");
	for (std::size_t piece = 1; piece <= 13; ++piece) {
		std::array<std::byte, vf::values.size()> joined{};
		std::size_t at = 0;
		resource::Cursor cursor = 0;
		bool ended = false;
		for (int guard = 0; !ended && guard < 1000; ++guard) {
			std::array<std::byte, 13> buffer{};
			const auto part = vf::values.read(cursor, std::span<std::byte>{buffer.data(), piece});
			// A piece shorter than the next whole token is refused, not split.
			if (part.status != resource::Status::Ok || at + part.written > joined.size())
				break;
			std::memcpy(joined.data() + at, buffer.data(), part.written);
			at += part.written;
			cursor = part.next;
			ended = part.eof;
		}
		if (piece >= vf::values.maxTokenSize())
			check(ended && at == whole.size() && joined == whole,
			      "pieced reads give the same bytes (a token is never split)");
	}

	// The header: magic, version 3.0, five Fields, the size, the tree's fingerprint.
	rs::ValuesHeader header;
	check(rs::readValuesHeader(whole, header) == rs::ValuesProblem::None,
	      "the header reads as a Values header");
	check(header.fieldCount == 5 && header.totalBytes == whole.size() &&
	          header.fingerprint == vf::descriptor.fingerprint(),
	      "the header names five Fields, the size and the tree descriptor");

	// The host side: the descriptor's RuntimeSchema reads every token back.
	namespace rt = ts::runtime;
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 32> members{};
	std::array<rt::RuntimeEnumEntry, 16> enums{};
	std::array<rt::RuntimeCatalog, 16> catalogs{};
	std::array<rt::RuntimeEndpoint, 16> endpoints{};
	rt::RuntimeSchemaWorkspace metadata{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(std::span<const std::byte>{vf::packed}, metadata);
	check(bool(parsed) && parsed.schema.groupTree(),
	      "the tree descriptor parses as a RuntimeSchema");
	if (!parsed)
		return 1;
	const auto& schema = parsed.schema;
	std::array<std::string_view, 5> names{};
	std::array<std::span<const std::byte>, 5> payloads{};
	std::size_t visited = 0;
	const auto problem = rs::readValues(whole, schema, [&](const rs::ValuesEntry& entry) {
		if (visited < names.size() && entry.status == rs::ValueStatus::Ok) {
			names[visited] = entry.endpoint->name;
			payloads[visited] = entry.payload;
		}
		++visited;
	});
	check(problem == rs::ValuesProblem::None && visited == 5, "the host reads five Field values");
	check(names[0] == "Config" && names[1] == "Status" && names[2] == "Gain" &&
	          names[3] == "Gain" && names[4] == "Config",
	      "tokens follow the groups in pre-order and each group's Fields in order");
	check(decoded<float>(payloads[2]) == *ts::read<vf::model, "adc/channel1/Gain">() &&
	          decoded<float>(payloads[3]) == *ts::read<vf::model, "adc/channel2/Gain">(),
	      "the two Gains are the values read by path");
	check(decoded<std::uint16_t>(payloads[0].subspan(4)) ==
	          *ts::read<vf::model, "adc/Config.rateHz">(),
	      "adc/Config's rateHz is the member read by path");
	check(decoded<std::int16_t>(payloads[1].subspan(4)) ==
	          *ts::read<vf::model, "adc/Status.raw[1]">(),
	      "adc/Status's raw[1] is the element read by path");
	check(decoded<std::uint32_t>(payloads[4]) == 10u &&
	          static_cast<std::uint8_t>(payloads[4][4]) ==
	              static_cast<std::uint8_t>(metrology::Window::Hann),
	      "metrology/Config is the other branch's Config");

	// A write by path is the next snapshot's value.
	check(ts::write<vf::model, "adc/channel2/Gain">(6.5f) == ts::WriteResult::Applied,
	      "write adc/channel2/Gain by path");
	check(vf::values.read(0, whole).status == resource::Status::Ok,
	      "read the file after the write");
	visited = 0;
	float gain = 0;
	check(rs::readValues(whole, schema,
	                     [&](const rs::ValuesEntry& entry) {
		                     if (visited++ == 3)
			                     gain = decoded<float>(entry.payload);
	                     }) == rs::ValuesProblem::None &&
	          gain == 6.5f,
	      "the next snapshot carries the written Gain");

	std::printf("Values of a group tree: checks=%d failures=%d\n", checks, failures);
	return failures == 0 ? 0 : 1;
}

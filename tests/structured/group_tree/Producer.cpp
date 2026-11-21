/*
 * @file Producer.cpp
 * @brief The group-tree Model composed from two branch files: bytes, dispatch and paths.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Model{adc::tlm::branch, metrology::tlm::branch} mounts two branches written
 * without knowing each other, each with its own Config. run.py compares the
 * written descriptor with tree.hex. The program checks the flattened outline,
 * the shared PackedIds and O(1) dispatch through the unchanged ModelView, the
 * flat Model over the same tables (v3.0 bytes), the registry's scoped type
 * names, and typed access by path: read<model, path>() of a Field, a member
 * and an element, write<> of a whole Field, call<> of a Command and a
 * Service, idOf<> and the consteval model.idOf<>().
 */

#include "AdcBranch.hpp"
#include "MetrologyBranch.hpp"

#include <telemetry/abi/StructuredAbi.hpp>
#include <telemetry/descriptor/Descriptor.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace tree_fixture {
namespace ts = telemetry;

inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};
using Tree = std::remove_cvref_t<decltype(model)>;
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = ::telemetry::descriptor::packDescriptor<descriptor>();

// The outline: adc, adc/channel1, adc/channel2, metrology, in pre-order.
static_assert(Tree::groupTree && Tree::groupCount == 4);
static_assert(Tree::groupDepths == std::array<std::uint16_t, 4>{0, 1, 1, 0});
static_assert(Tree::groupParents ==
              std::array<std::uint32_t, 4>{Tree::noParent, 0, 0, Tree::noParent});
static_assert(std::string_view{model.groupName(1)} == "channel1");

// One PackedId space: the group part is the shared pre-order position.
static_assert(ts::idOf<model, "adc/Config">() == ts::makeId(0, 0));
static_assert(ts::idOf<model, "adc/Status">() == ts::makeId(0, 1));
static_assert(ts::idOf<model, "adc/channel1/Gain">() == ts::makeId(1, 0));
static_assert(ts::idOf<model, "adc/channel2/Gain">() == ts::makeId(2, 0));
static_assert(ts::idOf<model, "metrology/Config">() == ts::makeId(3, 0));
static_assert(ts::idOf<model, "adc/Reset">() == ts::makeId(0, 0));
static_assert(ts::idOf<model, "adc/Calibrate">() == ts::makeId(0, 0));
static_assert(model.idOf<"adc/channel2/Gain">() == ts::makeId(2, 0));
static_assert(model.idOf<"metrology/Config">() == ts::makeId(3, 0));

// Two Configs, two names: the innermost scope keeps them apart.
static_assert(Tree::Registry::typeNames()[Tree::typeId<adc::Config>()] == "adc::Config");
static_assert(Tree::Registry::typeNames()[Tree::typeId<metrology::Config>()] ==
              "metrology::Config");
static_assert(Tree::typeId<adc::Config>() != Tree::typeId<metrology::Config>());

// The same tables as a flat Model are v3.0: no tree, and the group names are
// per-family catalogs again.
inline constexpr ts::Model flat{model.fields(), model.commands(), model.services()};
inline constexpr auto flatDescriptor = ::telemetry::descriptor::withoutTypeNames(flat);
static_assert(flatDescriptor.valid());
static_assert(::telemetry::descriptor::withoutTypeNames(model).valid());

// The in-memory ABI is unchanged by the tree.
static_assert(ts::structuredAbiRevision == 6);

// Paths: a Field, a member, an element, and their types.
static_assert(
    std::is_same_v<decltype(ts::read<model, "adc/channel1/Gain">()), std::optional<float>>);
static_assert(
    std::is_same_v<decltype(ts::read<model, "adc/Config">()), std::optional<adc::Config>>);
static_assert(
    std::is_same_v<decltype(ts::read<model, "adc/Config.rateHz">()), std::optional<std::uint16_t>>);
static_assert(
    std::is_same_v<decltype(ts::read<model, "adc/Status.raw[1]">()), std::optional<std::int16_t>>);
static_assert(std::is_same_v<decltype(ts::read<model, "metrology/Config.window">()),
                             std::optional<metrology::Window>>);

// Named handles: the path resolved once, the PackedId and value type in the type.
inline constexpr auto gain1 = ts::endpoint<model, "adc/channel1/Gain">;
inline constexpr auto gain2 = ts::endpoint<model, "adc/channel2/Gain">;
inline constexpr auto config = ts::endpoint<model, "adc/Config">;
inline constexpr auto rate = ts::endpoint<model, "adc/Config.rateHz">;
inline constexpr auto raw1 = ts::endpoint<model, "adc/Status.raw[1]">;
inline constexpr auto meterConfig = ts::endpoint<model, "metrology/Config">;
inline constexpr auto reset = ts::endpoint<model, "adc/Reset">;
inline constexpr auto calibrate = ts::endpoint<model, "adc/Calibrate">;
using Kind = ts::reflection::EndpointKind;
static_assert(gain1.id == ts::idOf<model, "adc/channel1/Gain">() && gain2.id == ts::makeId(2, 0));
static_assert(config.id == ts::makeId(0, 0) && rate.id == config.id && raw1.id == ts::makeId(0, 1));
static_assert(meterConfig.id == ts::makeId(3, 0) && reset.id == ts::makeId(0, 0) &&
              calibrate.id == ts::makeId(0, 0));
static_assert(gain1.kind == Kind::Field && reset.kind == Kind::Command &&
              calibrate.kind == Kind::Service);
static_assert(!gain1.member && !config.member && rate.member && raw1.member && !reset.member);
static_assert(std::is_same_v<decltype(gain1)::Value, float> &&
              std::is_same_v<decltype(config)::Value, adc::Config> &&
              std::is_same_v<decltype(rate)::Value, std::uint16_t> &&
              std::is_same_v<decltype(raw1)::Value, std::int16_t> &&
              std::is_same_v<decltype(reset)::Value, void>);
static_assert(std::is_same_v<decltype(calibrate)::Definition::Request, adc::Reference> &&
              std::is_same_v<decltype(calibrate)::Definition::Response, adc::Offset>);
// Empty and trivially copyable: a handle is its type, nothing at run time.
static_assert(std::is_empty_v<decltype(gain1)> && std::is_trivially_copyable_v<decltype(rate)>);
// The same endpoint by another declaration is the same handle type.
static_assert(std::is_same_v<decltype(gain1), decltype(ts::endpoint<model, "adc/channel1/Gain">)>);
static_assert(!std::is_same_v<decltype(gain1), decltype(gain2)>);
// A handle returns what the free functions return.
static_assert(
    std::is_same_v<decltype(gain1.read()), decltype(ts::read<model, "adc/channel1/Gain">())>);
static_assert(
    std::is_same_v<decltype(rate.read()), decltype(ts::read<model, "adc/Config.rateHz">())>);
static_assert(std::is_same_v<decltype(reset.call()), decltype(ts::call<model, "adc/Reset">())>);
static_assert(std::is_same_v<decltype(calibrate.call(adc::Reference{})),
                             decltype(ts::call<model, "adc/Calibrate">(adc::Reference{}))>);

} // namespace tree_fixture

namespace {
namespace tf = tree_fixture;
namespace ts = telemetry;

int failures = 0;

void check(bool condition, const char* what)
{
	if (!condition) {
		++failures;
		std::printf("FAIL %s\n", what);
	}
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	std::FILE* file = std::fopen(argv[1], "wb");
	if (!file)
		return 2;
	const bool written =
	    std::fwrite(tf::bytes.data(), 1, tf::bytes.size(), file) == tf::bytes.size();
	check(std::fclose(file) == 0 && written, "write the tree descriptor");

	// O(1) dispatch through the unchanged ModelView: G rows in every family,
	// an empty row where a group has none of that family.
	const auto view = tf::model.view();
	check(view.fieldCatalogCount == 4 && view.commandCatalogCount == 4 &&
	          view.serviceCatalogCount == 4,
	      "every family has one row per group");
	check(view.commands.catalogs()[1].count == 0 && view.services.catalogs()[3].count == 0,
	      "empty rows where a group has none of a family");
	check(view.fieldTypeId(ts::makeId(3, 0)) == tf::Tree::typeId<metrology::Config>(),
	      "metrology/Config dispatches to metrology::Config");
	check(!view.fieldTypeId(ts::makeId(4, 0)) && !view.fieldTypeId(ts::makeId(1, 1)),
	      "out-of-range group or entry is refused");
	std::array<std::byte, 64> scratch{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, 16> out{};
	const auto encoded = view.fields.readEncoded(ts::makeId(2, 0), out, workspace);
	float gain = 0;
	std::memcpy(&gain, out.data(), sizeof gain);
	check(encoded.dispatch == ts::DispatchStatus::Ok && encoded.written == 4 && gain == 2.5f,
	      "readEncoded adc/channel2/Gain");

	// Typed access by path.
	check(ts::read<tf::model, "adc/channel1/Gain">() == 1.5f, "read a Field by path");
	check(ts::read<tf::model, "adc/Config.rateHz">() == std::uint16_t{1000}, "read a member");
	check(ts::read<tf::model, "adc/Status.raw[1]">() == std::int16_t{7}, "read an element");
	check(ts::write<tf::model, "adc/channel2/Gain">(4.0f) == ts::WriteResult::Applied &&
	          ts::read<tf::model, "adc/channel2/Gain">() == 4.0f,
	      "write a whole Field by path");
	check(ts::write<tf::model, "metrology/Config">(
	          metrology::Config{20, metrology::Window::FlatTop}) == ts::WriteResult::Applied &&
	          ts::read<tf::model, "metrology/Config.cycles">() == 20u,
	      "write the other branch's Config");
	check(ts::call<tf::model, "adc/Reset">() == ts::CommandResult::Executed &&
	          adc::tlm::instance.resets == 1,
	      "call a Command by path");
	const auto offset = ts::call<tf::model, "adc/Calibrate">(adc::Reference{1.25f});
	check(offset.hasValue() && offset.value().counts == 2.5f &&
	          adc::tlm::instance.calibrations == 1,
	      "call a Service by path");

	// The same endpoints through named handles, and a stored copy of one.
	check(tf::gain1.read() == 1.5f, "read a Field through its handle");
	const auto stored = tf::gain2;
	check(stored.write(5.0f) == ts::WriteResult::Applied && tf::gain2.read() == 5.0f &&
	          ts::read<tf::model, "adc/channel2/Gain">() == 5.0f,
	      "write through a stored handle, read back by path");
	check(tf::rate.read() == std::uint16_t{1000} && tf::raw1.read() == std::int16_t{7},
	      "read a member and an element through their handles");
	check(tf::config.write(adc::Config{0.5f, 2000, false}) == ts::WriteResult::Applied &&
	          tf::rate.read() == std::uint16_t{2000} &&
	          ts::read<tf::model, "adc/Config.enabled">() == false,
	      "write a struct Field through its handle, read its members");
	check(tf::meterConfig.read()->window == metrology::Window::FlatTop,
	      "the other branch's Config through its handle");
	check(tf::reset.call() == ts::CommandResult::Executed && adc::tlm::instance.resets == 2,
	      "call a Command through its handle");
	const auto doubled = tf::calibrate.call(adc::Reference{3.0f});
	check(doubled.hasValue() && doubled.value().counts == 6.0f &&
	          adc::tlm::instance.calibrations == 2,
	      "call a Service through its handle with its argument");

	// The flat Model over the same tables writes v3.0 bytes with the same counts.
	check(tf::flatDescriptor.view().size() > 0, "flat descriptor of the same tables");
	std::printf("tree descriptor %zu bytes, fingerprint %016llx, flat %u bytes\n", tf::bytes.size(),
	            static_cast<unsigned long long>(tf::descriptor.fingerprint()),
	            tf::flatDescriptor.size());
	std::printf("group tree: producer %s\n", failures == 0 ? "ok" : "failed");
	return failures == 0 ? 0 : 1;
}

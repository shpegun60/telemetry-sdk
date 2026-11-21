/* Current public API, wire constants, default ceilings and dependency-pin contract.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT. */
#include "../traversal/Fixture.hpp"
#include <telemetry/descriptor/Format.hpp>
#include <integration/telemetry/v3/ValuesFormat.hpp>
#include <concepts>
#ifndef FREEZE_ARM
#include <cstdio>
#endif

namespace ts = telemetry;
namespace rs = resource::telemetry::v3;
using fixture::Config;
using fixture::Position;

// These facts describe the public contract, independent of C++ struct padding.
static_assert(std::same_as<telemetry::PackedId, std::uint32_t>);
static_assert(std::same_as<ts::TypeId, std::uint32_t>);
static_assert(telemetry::detail::makeId(2u, 7u) == 0x00020007u);
static_assert(ts::idOf<fixture::model, "repeat/Config">() == 0x00020007u);
static_assert(ts::structuredAbiRevision == 6);
static_assert(TELEMETRY_STRUCTURED_LOCAL_BYTES == 32);
static_assert(::telemetry::descriptor::binaryMajor == 3 &&
              ::telemetry::descriptor::binaryMinor == 0);
static_assert(::telemetry::descriptor::descriptorHeaderBytes == 64 && rs::valuesHeaderBytes == 24);
static_assert(::telemetry::descriptor::recordVersion == 1 &&
              ::telemetry::descriptor::recordHeaderBytes == 8);
static_assert(::telemetry::descriptor::fingerprintBasis == 0xcbf29ce484222325ULL);
static_assert(::telemetry::descriptor::fingerprintPrime == 0x100000001b3ULL);
static_assert(static_cast<unsigned>(::telemetry::descriptor::RecordKind::Type) == 1);
static_assert(static_cast<unsigned>(::telemetry::descriptor::RecordKind::Catalog) == 2);
static_assert(static_cast<unsigned>(::telemetry::descriptor::RecordKind::Field) == 3);
static_assert(static_cast<unsigned>(::telemetry::descriptor::RecordKind::Command) == 4);
static_assert(static_cast<unsigned>(::telemetry::descriptor::RecordKind::Service) == 5);
static_assert(static_cast<unsigned>(::telemetry::descriptor::Category::Field) == 1);
static_assert(static_cast<unsigned>(::telemetry::descriptor::Category::Command) == 2);
static_assert(static_cast<unsigned>(::telemetry::descriptor::Category::Service) == 3);
static_assert(static_cast<unsigned>(::telemetry::descriptor::Capability::Readable) == 1);
static_assert(static_cast<unsigned>(::telemetry::descriptor::Capability::Writable) == 2);
static_assert(static_cast<unsigned>(rs::ValueStatus::Ok) == 0);
static_assert(static_cast<unsigned>(rs::ValueStatus::Unavailable) == 1);
static_assert(static_cast<unsigned>(ts::TypeKind::Void) == 0);
static_assert(static_cast<unsigned>(ts::TypeKind::Scalar) == 1);
static_assert(static_cast<unsigned>(ts::TypeKind::Enum) == 2);
static_assert(static_cast<unsigned>(ts::TypeKind::Struct) == 3);
static_assert(static_cast<unsigned>(ts::TypeKind::Array) == 4);
static_assert(static_cast<unsigned>(ts::ScalarCode::Bool) == 1);
static_assert(static_cast<unsigned>(ts::ScalarCode::U8) == 2);
static_assert(static_cast<unsigned>(ts::ScalarCode::S8) == 3);
static_assert(static_cast<unsigned>(ts::ScalarCode::U16) == 4);
static_assert(static_cast<unsigned>(ts::ScalarCode::S16) == 5);
static_assert(static_cast<unsigned>(ts::ScalarCode::U32) == 6);
static_assert(static_cast<unsigned>(ts::ScalarCode::S32) == 7);
static_assert(static_cast<unsigned>(ts::ScalarCode::U64) == 8);
static_assert(static_cast<unsigned>(ts::ScalarCode::S64) == 9);
static_assert(static_cast<unsigned>(ts::ScalarCode::F32) == 10);
static_assert(static_cast<unsigned>(ts::ScalarCode::F64) == 11);
static_assert(ts::TypeRegistry<>::typeId<void>() == 0);
static_assert(ts::TypeRegistry<>::typeId<bool>() == 1);
static_assert(ts::TypeRegistry<>::typeId<std::uint32_t>() == 6);
static_assert(ts::TypeRegistry<>::typeId<float>() == 10);
static_assert(ts::TypeRegistry<>::typeId<double>() == 11);
static_assert(ts::wireSize<Config> == 5 && ts::wireSize<fixture::Mode> == 2);
static_assert(ts::wireSize<fixture::Big> == 4096);

using Fields = decltype(fixture::localFields);
using Commands = decltype(fixture::localCommands);
using Services = decltype(fixture::localServices);
static_assert(!std::copy_constructible<Fields> && !std::move_constructible<Fields>);
static_assert(!std::copy_constructible<Commands> && !std::move_constructible<Commands>);
static_assert(!std::copy_constructible<Services> && !std::move_constructible<Services>);
static_assert(!std::copy_constructible<ts::Workspace>);
static_assert(
    std::same_as<decltype(fixture::localFields.read<Position::Boolean>()), std::optional<bool>>);
static_assert(
    std::same_as<decltype(fixture::localFields.read<Position::Float>()), std::optional<double>>);
static_assert(std::same_as<decltype(fixture::localFields.read<Position::Mode>()),
                           std::optional<fixture::Mode>>);
static_assert(std::same_as<decltype(fixture::localFields.read<Position::Array>()),
                           std::optional<std::array<std::uint16_t, 2>>>);
static_assert(
    std::same_as<decltype(ts::read<fixture::model, "repeat/Config">()), std::optional<Config>>);
static_assert(std::same_as<decltype(fixture::fields.readAs<double>(7u)), std::optional<double>>);
static_assert(
    std::same_as<decltype(fixture::fields.writeAs(7u, Config{})), telemetry::WriteResult>);
static_assert(std::same_as<decltype(fixture::localFields.readAs<double, Position::U16>()),
                           std::optional<double>>);
static_assert(std::same_as<decltype(ts::endpoint<fixture::model, "repeat/Config">.read()),
                           std::optional<Config>>);
static_assert(std::same_as<decltype(fixture::localFields.writeAs<Position::U16>(1.0)),
                           telemetry::WriteResult>);
static_assert(std::same_as<decltype(ts::write<fixture::model, "first/Config">(Config{})),
                           telemetry::WriteResult>);
static_assert(std::same_as<decltype(fixture::model.view()), ts::ModelView>);
static_assert(
    std::same_as<decltype(fixture::commands.call<0>(Config{})), telemetry::CommandResult>);
static_assert(std::same_as<decltype(fixture::commands.call<1>()), telemetry::CommandResult>);
static_assert(
    std::same_as<decltype(fixture::services.call<0>(Config{})), ts::ServiceResult<Config>>);
static_assert(std::same_as<decltype(fixture::services.call<1>()), ts::ServiceResult<void>>);
static_assert(fixture::model.view().fieldTypeId(7u) == fixture::model.view().commandTypeId(0u));
static_assert(
    std::same_as<std::remove_cvref_t<decltype(fixture::fields.get<0x00020007u>())>,
                 std::remove_cvref_t<decltype(fixture::localFields.get<Position::Config>())>>);
static_assert(std::same_as<std::remove_cvref_t<decltype(fixture::commands.get<0x00020000u>())>,
                           std::remove_cvref_t<decltype(fixture::localCommands.get<0>())>>);
static_assert(std::same_as<std::remove_cvref_t<decltype(fixture::services.get<0x00020000u>())>,
                           std::remove_cvref_t<decltype(fixture::localServices.get<0>())>>);
static_assert(
    std::same_as<decltype(ts::call<fixture::model, "first/Reset">()), telemetry::CommandResult>);
static_assert(std::same_as<decltype(ts::call<fixture::model, "first/Echo">(Config{})),
                           ts::ServiceResult<Config>>);
static_assert(fixture::noFields.begin() == fixture::noFields.end());
static_assert(fixture::noCommands.begin() == fixture::noCommands.end());
static_assert(fixture::noServices.begin() == fixture::noServices.end());

template<class T>
concept SemanticMetadata =
    requires(T value) { value.unit; } || requires(T value) { value.min; } ||
    requires(T value) { value.max; } || requires(T value) { value.defaultValue; } ||
    requires(T value) { value.limits; } || requires(T value) { value.defaults; } ||
    requires(T value) { value.step; } || requires(T value) { value.constraints; } ||
    requires(T value) { value.argumentMetadata; } || requires(T value) { value.memberMetadata; };

// Positive controls make the new clauses meaningful even while production
// descriptors correctly contain none of these members.
struct StepMetadata {
	float step;
};

struct ConstraintsMetadata {
	unsigned constraints;
};

struct LimitsMetadata {
	unsigned limits;
};

struct DefaultsMetadata {
	unsigned defaults;
};

static_assert(SemanticMetadata<StepMetadata>);
static_assert(SemanticMetadata<ConstraintsMetadata>);
static_assert(SemanticMetadata<LimitsMetadata>);
static_assert(SemanticMetadata<DefaultsMetadata>);
static_assert(!SemanticMetadata<ts::TypeDescriptor>);
static_assert(!SemanticMetadata<ts::FieldEntry>);
static_assert(!SemanticMetadata<ts::CommandEntry>);
static_assert(!SemanticMetadata<ts::ServiceEntry>);

// Both traversal views borrow the existing rows. None reads live values.
template<class Table, class Check>
void checkTraversal(const Table& table, Check& check)
{
	std::size_t count = 0;
	bool ordered = true;
	table.forEach([&]<std::size_t I>(const auto& definition) {
		ordered = ordered && I == count && definition.name() == table[I].name;
		++count;
	});
	check(ordered && count == table.size());
	count = 0;
	for (const auto& entry : table)
		ordered = ordered && &entry == table.data() + count++;
	check(ordered && count == table.size() && table.end() == table.data() + count);
	const auto& expected = table.template get<0>();
	bool same = false;
	check(table.visit(0, [&](const auto& definition) {
		same = static_cast<const void*>(&definition) == &expected;
	}) && same);
	check(!table.visit(table.size(), [](const auto&) {
	}));
}

int main()
{
	unsigned checks = 0, failures = 0;
	const auto check = [&](bool value) {
		++checks;
		if (!value)
			++failures;
	};
	checkTraversal(fixture::localFields, check);
	checkTraversal(fixture::localCommands, check);
	checkTraversal(fixture::localServices, check);
	check(fixture::device.reads == 0 && fixture::device.writes == 0 &&
	      fixture::device.commands == 0 && fixture::device.services == 0);
	const Config request{0x12345678u, true};
	std::array<std::byte, 5> bytes{};
	const std::array expected{std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
	                          std::byte{1}};
	check(ts::encode(request, bytes) == ts::CodecStatus::Ok);
	check(bytes == expected);
	check(fixture::localFields.write<Position::Config>(request) == telemetry::WriteResult::Applied);
	const auto value = fixture::fields.readAs<Config>(ts::idOf<fixture::model, "repeat/Config">());
	check(value && value->code == request.code);
	check(fixture::commands.call<1>() == telemetry::CommandResult::Executed);
	const auto response = fixture::services.call<0>(request);
	check(response.status() == ts::ServiceStatus::Ok && response.value().code == request.code);
	ts::Workspace empty{std::span<std::byte>{}};
	check(fixture::fields.index().readEncoded(7u, bytes, empty).dispatch == ts::DispatchStatus::Ok);
	check(bytes == expected && empty.used() == 0);
	bytes.back() = std::byte{2};
	const auto writes = fixture::device.writes;
	check(fixture::fields.index().writeEncoded(7u, bytes, empty).dispatch ==
	      ts::DispatchStatus::InvalidPayload);
	check(fixture::device.writes == writes);
	check(!fixture::fields.readAs<Config>(0x10000u));
	check(!fixture::fields.readAs<Config>(0x100000000ULL));
	check(!fixture::fields.readAs<float>(7u));
	check(fixture::fields.writeAs(7u, std::array<std::uint16_t, 2>{}) ==
	      telemetry::WriteResult::InvalidValue);
	check(fixture::fields.writeAs(1u, 12.75) == telemetry::WriteResult::Applied);
	check(fixture::fields.readAs<double>(1u) == 12.0);
#ifndef FREEZE_ARM
	std::printf("{\"checks\":%u,\"failures\":%u}\n", checks, failures);
#endif
	return failures == 0 ? 0 : 1;
}

/*
 * @file Negative.cpp
 * @brief Rejected signatures, lifetime escapes, metadata and routing mistakes.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "MixedFixture.hpp"
using namespace fixture;
using WR = telemetry::WriteResult;
using CR = telemetry::CommandResult;

MotorConfig get() noexcept
{
	return {};
}

WR set(const MotorConfig&) noexcept
{
	return WR::Applied;
}

struct Owner {
	MotorConfig read() const noexcept
	{
		return {};
	}

	WR write(const MotorConfig&) noexcept
	{
		return WR::Applied;
	}

	CR call(const MotorConfig&) noexcept
	{
		return CR::Executed;
	}
};

struct Callable {
	int state;

	MotorConfig operator()() const noexcept
	{
		return {};
	}
};

// A real value object exercises the metadata refusal, without relying on
// deleted legacy declarations to produce an unrelated missing-header error.
struct ValueMetadata {
	float initial, minimum, maximum;
};

#if CASE == 1
auto invalid = ts::field("bad", [](int) noexcept {
	return 1;
});
#elif CASE == 2
auto invalid = ts::field("bad", []() noexcept -> MotorConfig& {
	return device.current;
});
#elif CASE == 3
auto invalid = ts::field("bad", []() {
	return 1;
});
#elif CASE == 4
auto invalid = ts::field("bad", &get, [](double) noexcept {
	return WR::Applied;
});
#elif CASE == 5
auto invalid = ts::field("bad", &get, [](MotorConfig) noexcept {
	return true;
});
#elif CASE == 6
auto invalid = ts::field("bad", &get, [](MotorConfig&) noexcept {
	return WR::Applied;
});
#elif CASE == 7
auto invalid = ts::command("bad", [](float) noexcept {
	return CR::Executed;
});
#elif CASE == 8
auto invalid = ts::command("bad", [](MotorConfig) noexcept {
	return 1;
});
#elif CASE == 9
auto invalid = ts::command("bad", [](MotorConfig, int) noexcept {
	return CR::Executed;
});
#elif CASE == 10
auto invalid = ts::command("bad", [](const volatile MotorConfig&) noexcept {
	return CR::Executed;
});
#elif CASE == 11
auto invalid = ts::command("bad", [](MotorConfig) {
	return CR::Executed;
});
#elif CASE == 12
auto invalid = ts::field<&Owner::read>("bad", Owner{});
#elif CASE == 13
auto invalid = ts::field<&Owner::read, const Owner>("bad", {});
#elif CASE == 14
auto invalid = ts::field("bad", Callable{});
#elif CASE == 15
auto invalid = ts::command<&Owner::call>("bad", Owner{});
#elif CASE == 16
auto invalid = ts::command<&Owner::call, const Owner>("bad", {});
#elif CASE == 17
auto invalid = mixedFields.read<8>();
#elif CASE == 18
auto invalid = ts::testing::CatalogAccess::read<ts::detail::makeId<1, 0>()>(v30Fields);
#elif CASE == 19
auto invalid = localCommands.call<2>();
#elif CASE == 20
auto invalid = ts::testing::CatalogAccess::call<ts::detail::makeId<1, 0>()>(v30Commands);
#elif CASE == 21
auto invalid = mixedFields.write<7>(12.0f);
#elif CASE == 22
auto invalid = localCommands.call<0>(12.0f);
#elif CASE == 23
auto invalid = mixedFields;
#elif CASE == 24
auto invalid = model;
#elif CASE == 25
auto invalid = ts::testing::group("bad", ts::FieldTable{ts::field<&get>("f")});
#elif CASE == 26
auto invalid = ts::testing::FieldCatalogTable{ts::testing::group("bad", localCommands)};
#elif CASE == 27
auto invalid = v30Fields.read<Position::Config>();
#elif CASE == 28
auto invalid = ts::field("bad", "V", &get);
#elif CASE == 29
auto invalid = ts::field("bad", &get, ValueMetadata{1, 0, 2});
#elif CASE == 30
auto invalid = ts::command(
    "bad", "V", +[]() noexcept {
	    return CR::Executed;
    });
#elif CASE == 31
auto invalid = ts::field<static_cast<MotorConfig (*)() noexcept>(nullptr)>("bad");
#elif CASE == 32
auto invalid = ts::command<static_cast<CR (*)() noexcept>(nullptr)>("bad");
#elif CASE == 33
auto invalid = ts::field<&Owner::read, &Owner::write, const Owner>("bad", {Owner{}});
#elif CASE == 34
auto invalid = ts::testing::V30Model{v30Fields, v30Commands, ts::testing::ServiceCatalogTable{}};
#elif CASE == 35
auto invalid = model.fieldIndex().find<std::uint16_t>(65537);
#elif CASE == 36
auto invalid = model.commandIndex().executeEncoded<std::uint16_t>(
    65537, {}, *static_cast<ts::Workspace*>(nullptr));
#elif CASE == 37
auto invalid = ts::testing::group<const ts::FieldTable<>>("bad", ts::FieldTable<>{});
#elif CASE == 38
auto invalid = ts::testing::group<const ts::CommandTable<>>("bad", {ts::CommandTable<>{}});
#elif CASE == 39
ts::testing::V30Model<ts::testing::FieldCatalogTable<>, ts::testing::CommandCatalogTable<>,
                      ts::testing::ServiceCatalogTable<>>
    invalid{{ts::testing::FieldCatalogTable<>{}},
            {ts::testing::CommandCatalogTable<>{}},
            {ts::testing::ServiceCatalogTable<>{}}};
#elif CASE == 40
ts::testing::V30Model<ts::testing::EmptyEndpointCatalog, ts::testing::EmptyEndpointCatalog,
                      std::remove_cv_t<decltype(v30Services)>>
    invalid{{}, ts::testing::emptyCommands, v30Services};
#else
#error Select a negative case
#endif

/*
 * @file Negative.cpp
 * @brief Invalid names, duplicate declarations and bounded descriptor profiles.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "Fixture.hpp"
namespace ts = telemetry;
namespace df = descriptor_fixture;
#if CASE == 1
constexpr auto bad = ts::field<&df::read<std::uint32_t>>("bad\0tail");
#elif CASE == 2
constexpr auto bad = ts::command<&fixture::Device::reset>("\xc0\x80", fixture::device);
#elif CASE == 3
constexpr auto bad = ts::service<&df::ping>("");
#elif CASE == 4
constexpr auto bad = ts::testing::group("a\0b", df::extraFields);
#elif CASE == 5
constexpr auto bad = ts::field<&df::read<std::uint32_t>>(nullptr);
#elif CASE == 6
constexpr auto bad = ts::field<&df::read<std::uint32_t>>("\xed\xa0\x80");
#elif CASE == 7
constexpr auto bad = ts::service<&df::ping>("\xf4\x90\x80\x80");
#elif CASE == 8
constexpr auto bad = ts::testing::group("\xe2\x82", df::extraFields);
#elif CASE == 9
inline constexpr ts::FieldTable rows{ts::field<&df::read<int>>("x"),
                                     ts::field<&df::read<int>>("x")};
inline constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("g", rows)};
inline constexpr ts::testing::V30Model model{catalogs, ts::testing::emptyCommands, df::noServices};
constexpr ::telemetry::descriptor::Descriptor bad{model};
#elif CASE == 10
inline constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("g", df::extraFields),
                                                         ts::testing::group("g", df::noFields)};
inline constexpr ts::testing::V30Model model{catalogs, ts::testing::emptyCommands, df::noServices};
constexpr ::telemetry::descriptor::Descriptor bad{model};
#elif CASE == 11
auto bad = ::telemetry::descriptor::Descriptor{df::model}.view();
#else
struct Profile : ts::Limits {
#if CASE == 12
	static constexpr unsigned maxTypeCount = 12;
#elif CASE == 13
	static constexpr unsigned maxDescriptorBytes = 500;
#elif CASE == 14
	static constexpr unsigned maxStringBytes = 3;
#elif CASE == 15
	static constexpr unsigned maxEnumEntriesTotal = 1;
#elif CASE == 16
	static constexpr unsigned maxStructMembers = 2;
#elif CASE == 17
	static constexpr unsigned maxArrayElements = 1;
#elif CASE == 18
	static constexpr unsigned maxTypeDepth = 1;
#elif CASE == 19
	static constexpr unsigned maxExpandedValueNodes = 3;
#elif CASE == 20
	static constexpr unsigned maxValueWireBytes = 8;
#elif CASE == 21
	static constexpr unsigned maxEndpointCountTotal = 1;
#elif CASE == 22
	static constexpr unsigned maxCatalogCountTotal = 1;
#else
#error Unknown CASE
#endif
};

constexpr ::telemetry::descriptor::Descriptor<std::remove_cv_t<decltype(df::fields)>,
                                              std::remove_cv_t<decltype(fixture::v30Commands)>,
                                              std::remove_cv_t<decltype(df::services)>, Profile>
    bad{df::model};
#endif

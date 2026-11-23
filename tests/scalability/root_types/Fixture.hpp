/*
 * @file Fixture.hpp
 * @brief Repeated positional roots, stable registry order and live value fixtures.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * One source supports the baseline and optimized composition paths. The test
 * switch enables new-alias assertions without changing endpoints or wire data.
 */
#ifndef TELEMETRY_TESTS_SCALABILITY_ROOT_TYPES_FIXTURE_HPP
#define TELEMETRY_TESTS_SCALABILITY_ROOT_TYPES_FIXTURE_HPP
#pragma once

#include <telemetry/Telemetry.hpp>
#ifndef TELEMETRY_TEST_MODULAR_LAYOUT
#define TELEMETRY_TEST_MODULAR_LAYOUT 1
#endif
#if TELEMETRY_TEST_MODULAR_LAYOUT
#include <integration/telemetry/v3/ValuesFile.hpp>
#else
#include <resource/telemetry/v3/ValuesFile.hpp>
#endif
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifndef TELEMETRY_TEST_REGISTRY_ROOTS
#define TELEMETRY_TEST_REGISTRY_ROOTS 0
#endif

namespace root_types_fixture {
namespace ts = ::telemetry;
namespace rs = ::resource::telemetry::v3;
#if TELEMETRY_TEST_MODULAR_LAYOUT
namespace descriptor_api = ::telemetry::descriptor;
#else
namespace descriptor_api = ::resource::telemetry::v3;
#endif

// Shared dependency; code and ready occupy three canonical bytes.
struct Leaf {
	std::uint16_t code;
	bool ready;
	friend bool operator==(const Leaf&, const Leaf&) = default;
};

// Shared exact payload used as Field value, Command request and Service payload.
struct Packet {
	Leaf leaf;
	std::array<std::uint8_t, 3> bytes;
	friend bool operator==(const Packet&, const Packet&) = default;
};

// Identical shape is deliberately a different C++ identity and TypeId.
struct SameShape {
	Leaf leaf;
	std::array<std::uint8_t, 3> bytes;
};

enum class Mode : std::uint8_t {
	Off = 0,
	On = 7
};

// These two roots appear first in a Service Request/Response pair.
struct Query {
	std::uint8_t mode;
};

struct Reply {
	Leaf leaf;
	bool done;
};

// Counted business owner; getters are stable across resource cursor retries.
// Public methods:
// - ownA()/ownB()/borrowA()/same()/modeA()/modeB()/secondB()/leaf(): Read Fields.
// - set(): Replace Packet.
// - reset()/accept()/acceptSame()/acceptLeaf(): Execute Commands.
// - echo()/lookup()/reverse()/borrow()/nothing()/convert()/borrowReply(): Call Services.
struct Device {
	Packet a{{0x1234, true}, {0x11, 0x22, 0x33}};
	Packet b{{0xabcd, false}, {0x44, 0x55, 0x66}};
	Reply reply{{0x3456, true}, false};
	std::array<unsigned, 8> reads{};
	unsigned writes = 0, commands = 0, services = 0;

	Packet ownA() noexcept
	{
		++reads[0];
		return a;
	}

	Packet ownB() noexcept
	{
		++reads[1];
		return b;
	}

	const Packet& borrowA() noexcept
	{
		++reads[2];
		return a;
	}

	SameShape same() noexcept
	{
		++reads[3];
		return {{0x1357, true}, {0x77, 0x88, 0x99}};
	}

	Mode modeA() noexcept
	{
		++reads[4];
		return Mode::On;
	}

	Mode modeB() noexcept
	{
		++reads[5];
		return Mode::Off;
	}

	Packet secondB() noexcept
	{
		++reads[6];
		return b;
	}

	Leaf leaf() noexcept
	{
		++reads[7];
		return a.leaf;
	}

	ts::WriteResult set(const Packet& next) noexcept
	{
		++writes;
		a = next;
		return ts::WriteResult::Applied;
	}

	ts::CommandResult reset() noexcept
	{
		++commands;
		return ts::CommandResult::Executed;
	}

	ts::CommandResult accept(const Packet&) noexcept
	{
		++commands;
		return ts::CommandResult::Executed;
	}

	ts::CommandResult acceptSame(const SameShape&) noexcept
	{
		++commands;
		return ts::CommandResult::Executed;
	}

	ts::CommandResult acceptLeaf(const Leaf&) noexcept
	{
		++commands;
		return ts::CommandResult::Executed;
	}

	Packet echo(const Packet& input) noexcept
	{
		++services;
		return input;
	}

	Reply lookup(const Query&) noexcept
	{
		++services;
		return reply;
	}

	Query reverse(const Reply&) noexcept
	{
		++services;
		return {7};
	}

	const Packet& borrow(const Packet&) noexcept
	{
		++services;
		return a;
	}

	void nothing() noexcept
	{
		++services;
	}

	SameShape convert(const Packet& input) noexcept
	{
		++services;
		return {input.leaf, input.bytes};
	}

	const Reply& borrowReply(const Query&) noexcept
	{
		++services;
		return reply;
	}
};

inline Device device;
inline constexpr ts::FieldTable fieldA{
    ts::field<&Device::ownA, &Device::set>("OwnA", device),
    ts::field<&Device::ownB>("OwnB", device), ts::field<&Device::borrowA>("BorrowA", device),
    ts::field<&Device::same>("SameShape", device), ts::field<&Device::modeA>("ModeA", device)};
inline constexpr ts::FieldTable fieldB{ts::field<&Device::modeB>("ModeB", device),
                                       ts::field<&Device::secondB>("SecondB", device),
                                       ts::field<&Device::leaf>("Leaf", device)};
inline constexpr ts::FieldTable<> emptyField{};
inline constexpr ts::FieldCatalogTable fields{
    ts::group("first", fieldA), ts::group("empty", emptyField), ts::group("last", fieldB)};

inline constexpr ts::CommandTable commandA{ts::command<&Device::reset>("Reset", device),
                                           ts::command<&Device::accept>("AcceptA", device),
                                           ts::command<&Device::accept>("AcceptB", device)};
inline constexpr ts::CommandTable commandB{ts::command<&Device::acceptSame>("AcceptSame", device),
                                           ts::command<&Device::reset>("ResetAgain", device),
                                           ts::command<&Device::acceptLeaf>("AcceptLeaf", device)};
inline constexpr ts::CommandTable<> emptyCommand{};
inline constexpr ts::CommandCatalogTable commands{
    ts::group("first", commandA), ts::group("empty", emptyCommand), ts::group("last", commandB)};

inline constexpr ts::ServiceTable serviceA{ts::service<&Device::echo>("Echo", device),
                                           ts::service<&Device::lookup>("Lookup", device),
                                           ts::service<&Device::reverse>("Reverse", device),
                                           ts::service<&Device::borrow>("Borrow", device),
                                           ts::service<&Device::nothing>("Nothing", device)};
inline constexpr ts::ServiceTable serviceB{
    ts::service<&Device::convert>("Convert", device),
    ts::service<&Device::borrowReply>("BorrowReply", device)};
inline constexpr ts::ServiceTable<> emptyService{};
inline constexpr ts::ServiceCatalogTable services{
    ts::group("first", serviceA), ts::group("empty", emptyService), ts::group("last", serviceB)};
inline constexpr ts::Model model{fields, commands, services};
inline constexpr descriptor_api::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = descriptor_api::packDescriptor<descriptor>();
inline std::array<std::byte, 128> scratch{};
inline ts::Workspace workspace{scratch};
inline constexpr rs::ValuesFile values{descriptor, workspace};

// Positional lists remain a declaration contract, independently of deduplication.
static_assert(std::is_same_v<typename decltype(fieldA)::RootTypes,
                             ts::detail::TypeList<Packet, Packet, Packet, SameShape, Mode>>);
static_assert(
    std::is_same_v<typename decltype(fieldB)::RootTypes, ts::detail::TypeList<Mode, Packet, Leaf>>);
static_assert(std::is_same_v<
              typename decltype(fields)::RootTypes,
              ts::detail::TypeList<Packet, Packet, Packet, SameShape, Mode, Mode, Packet, Leaf>>);
static_assert(std::is_same_v<typename decltype(commandA)::RootTypes,
                             ts::detail::TypeList<void, Packet, Packet>>);
static_assert(std::is_same_v<typename decltype(commandB)::RootTypes,
                             ts::detail::TypeList<SameShape, void, Leaf>>);
static_assert(std::is_same_v<typename decltype(serviceA)::RootTypes,
                             ts::detail::TypeList<Packet, Packet, Query, Reply, Reply, Query,
                                                  Packet, Packet, void, void>>);
static_assert(std::is_same_v<typename decltype(serviceB)::RootTypes,
                             ts::detail::TypeList<Packet, SameShape, Query, Reply>>);

// RootTypes-only application wrappers must retain source-compatible composition.
// Public methods:
// - index(): Borrow original index.
// - size(): Count original groups.
struct LegacyFields {
	using RootTypes = typename decltype(fields)::RootTypes;
	template<class Registry>
	using TypeStorage = typename decltype(fields)::template TypeStorage<Registry>;

	constexpr auto index() const noexcept
	{
		return fields.index();
	}

	constexpr auto size() const noexcept
	{
		return fields.size();
	}
};

inline constexpr LegacyFields legacyFields{};
inline constexpr ts::Model legacyModel{legacyFields, commands, services};
static_assert(legacyModel.types().count == model.types().count);
static_assert(legacyModel.typeId<Query>() == model.typeId<Query>());

#if TELEMETRY_TEST_REGISTRY_ROOTS
// New-alias controls leave endpoint definitions and expected wire data unchanged.
static_assert(std::is_same_v<typename decltype(fieldA)::RegistryRootTypes,
                             ts::detail::TypeList<Packet, SameShape, Mode>>);
static_assert(std::is_same_v<typename decltype(fieldB)::RegistryRootTypes,
                             ts::detail::TypeList<Mode, Packet, Leaf>>);
static_assert(std::is_same_v<typename decltype(fields)::RegistryRootTypes,
                             ts::detail::TypeList<Packet, SameShape, Mode, Leaf>>);
static_assert(std::is_same_v<typename decltype(commandA)::RegistryRootTypes,
                             ts::detail::TypeList<void, Packet>>);
static_assert(std::is_same_v<typename decltype(commandB)::RegistryRootTypes,
                             ts::detail::TypeList<SameShape, void, Leaf>>);
static_assert(std::is_same_v<typename decltype(commands)::RegistryRootTypes,
                             ts::detail::TypeList<void, Packet, SameShape, Leaf>>);
static_assert(std::is_same_v<typename decltype(serviceA)::RegistryRootTypes,
                             ts::detail::TypeList<Packet, Query, Reply, void>>);
static_assert(std::is_same_v<typename decltype(serviceB)::RegistryRootTypes,
                             ts::detail::TypeList<Packet, SameShape, Query, Reply>>);
static_assert(std::is_same_v<typename decltype(services)::RegistryRootTypes,
                             ts::detail::TypeList<Packet, Query, Reply, void, SameShape>>);
static_assert(decltype(emptyField)::RegistryRootTypes::size == 0);
static_assert(decltype(emptyCommand)::RegistryRootTypes::size == 0);
static_assert(decltype(emptyService)::RegistryRootTypes::size == 0);
static_assert(ts::EmptyEndpointCatalog::RegistryRootTypes::size == 0);

// A helper that merely moves N-root recursion elsewhere fails this control.
template<std::size_t>
using RepeatedPacket = const Packet&;
template<std::size_t... I>
auto repeatedRoots(std::index_sequence<I...>) -> ts::detail::TypeList<RepeatedPacket<I>...>;
using Repeated2048 = decltype(repeatedRoots(std::make_index_sequence<2048>{}));
static_assert(std::is_same_v<typename ts::detail::UniqueFirst<Repeated2048>::type,
                             ts::detail::TypeList<Packet>>);
static_assert(std::is_same_v<typename ts::detail::UniqueFirst<ts::detail::TypeList<>>::type,
                             ts::detail::TypeList<>>);
using CvRoots = ts::detail::TypeList<const Packet&, const SameShape&, void, Packet&&,
                                     volatile SameShape, Leaf, const Leaf&, const Packet>;
static_assert(std::is_same_v<typename ts::detail::UniqueFirst<CvRoots>::type,
                             ts::detail::TypeList<Packet, SameShape, void, Leaf>>);

// Distinct markers detect dropped or reordered block arguments. The 65-root
// case repeats earlier types across both block boundaries with cv/ref forms.
template<std::size_t>
struct Marker {};

template<std::size_t... I>
auto markers(std::index_sequence<I...>) -> ts::detail::TypeList<Marker<I>...>;
using Markers33 = decltype(markers(std::make_index_sequence<33>{}));
static_assert(std::is_same_v<typename ts::detail::UniqueFirst<Markers33>::type, Markers33>);
template<std::size_t I>
using BlockMarker =
    std::conditional_t<I == 32, const Marker<0>&,
                       std::conditional_t<I == 64, Marker<1>&&, Marker<(I > 32 ? I - 1 : I)>>>;
template<std::size_t... I>
auto blockMarkers(std::index_sequence<I...>) -> ts::detail::TypeList<BlockMarker<I>...>;
using Markers65 = decltype(blockMarkers(std::make_index_sequence<65>{}));
using ExpectedMarkers63 = decltype(markers(std::make_index_sequence<63>{}));
static_assert(std::is_same_v<typename ts::detail::UniqueFirst<Markers65>::type, ExpectedMarkers63>);
#endif

static_assert(descriptor.valid() && model.types().count == 19);
static_assert(model.typeId<Leaf>() == 12 && model.typeId<std::array<std::uint8_t, 3>>() == 13);
static_assert(model.typeId<Packet>() == 14 && model.typeId<SameShape>() == 15);
static_assert(model.typeId<Mode>() == 16 && model.typeId<Query>() == 17 &&
              model.typeId<Reply>() == 18);
static_assert(model.typeId<const Packet&>() == model.typeId<Packet>());
static_assert(values.fieldCount() == 8 && values.size() == 67);

inline constexpr std::array<std::uint32_t, 9> offsets{24, 31, 38, 45, 52, 54, 56, 63, 67};
inline constexpr std::array<std::uint8_t, 43> expectedTokens{
    0,    0x34, 0x12, 1,    0x11, 0x22, 0x33, 0,    0xcd, 0xab, 0,    0x44, 0x55, 0x66, 0,
    0x34, 0x12, 1,    0x11, 0x22, 0x33, 0,    0x57, 0x13, 1,    0x77, 0x88, 0x99, 0,    7,
    0,    0,    0,    0xcd, 0xab, 0,    0x44, 0x55, 0x66, 0,    0x34, 0x12, 1};

} // namespace root_types_fixture
#endif // TELEMETRY_TESTS_SCALABILITY_ROOT_TYPES_FIXTURE_HPP

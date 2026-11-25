/*
 * @file Scale.cpp
 * @brief Runtime-native and encoded reads of 128 distinct targets.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
#include "Fixture.hpp"

#ifdef MCU_SCALE
namespace mcu {
namespace {
struct Device {
	std::array<std::uint32_t, rows> values{};

	template<std::size_t I>
	std::uint32_t read() const noexcept
	{
		return values[I];
	}
};

Device device;

constexpr auto names() noexcept
{
	std::array<std::array<char, 5>, rows> result{};
	for (unsigned i = 0; i < rows; ++i)
		result[i] = {'v', char('0' + i / 100), char('0' + i / 10 % 10), char('0' + i % 10), '\0'};
	return result;
}

inline constexpr auto labels = names();

template<std::size_t... I>
constexpr auto table(std::index_sequence<I...>) noexcept
{
	return ts::FieldTable{ts::field<&Device::read<I>>(labels[I].data(), device)...};
}

template<std::size_t... I>
constexpr auto branch(std::index_sequence<I...>) noexcept
{
	return ts::group("scale", ts::field<&Device::read<I>>(labels[I].data(), device)...);
}

inline constexpr auto local = table(std::make_index_sequence<rows>{});
// The same Fields as one group of a tree; its catalog table selects a row by runtime ID.
inline constexpr ts::Model model{branch(std::make_index_sequence<rows>{})};
inline constexpr auto& fields = model.fields();

std::uint32_t direct(std::uint32_t id) noexcept
{
	return device.values[id];
}

std::uint32_t directKnown(std::uint32_t) noexcept
{
	return device.read<0>();
}

std::uint32_t typedLocal(std::uint32_t) noexcept
{
	return local.read<0>().value_or(0);
}

std::uint32_t typedGlobal(std::uint32_t) noexcept
{
	return ::telemetry::detail::CatalogAccess::read<0>(fields).value_or(0);
}

struct Visitor {
	std::uint32_t* result;

	template<class Endpoint>
	void operator()(const Endpoint& endpoint) const noexcept
	{
		*result = endpoint.read().value_or(0);
	}
};

std::uint32_t namedVisitor(std::uint32_t id) noexcept
{
	std::uint32_t result = 0;
	(void)fields.visit(id, Visitor{&result});
	return result;
}

std::uint32_t lambdaVisitor(std::uint32_t id) noexcept
{
	std::uint32_t result = 0;
	(void)fields.visit(id, [&](const auto& endpoint) {
		result = endpoint.read().value_or(0);
	});
	return result;
}

std::uint32_t nativeAs(std::uint32_t id) noexcept
{
	return fields.readAs<std::uint32_t>(id).value_or(0);
}

std::uint32_t encoded(std::uint32_t id) noexcept
{
	std::array<std::byte, 4> bytes;
	ts::Workspace workspace{std::span<std::byte>{}};
	const auto result = model.fieldIndex().readEncoded(id, bytes, workspace);
	if (result.dispatch != ts::DispatchStatus::Ok)
		return 0;
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t(std::to_integer<unsigned>(bytes[i])) << (8 * i);
	return value;
}

constexpr Operation probes[]{
    {"direct_index", direct, 7, 4096},          {"local_u32", typedLocal, 1, 4096},
    {"global_u32", typedGlobal, 1, 4096},       {"named_visitor", namedVisitor, 7, 4096},
    {"lambda_visitor", lambdaVisitor, 7, 4096}, {"readAs_u32", nativeAs, 7, 4096},
    {"encoded_u32", encoded, 7, 4096},          {"direct_known_u32", directKnown, 1, 4096}};
} // namespace

std::span<const Operation> operations() noexcept
{
	return probes;
}

void prepare() noexcept
{
	for (unsigned i = 0; i < rows; ++i)
		device.values[i] = 17 + i * 3;
}

std::uint32_t expected(unsigned operation, std::uint32_t id) noexcept
{
	return 17 + (((operation >= 1 && operation <= 2) || operation == 7) ? 0 : id * 3);
}

void checkProbes() noexcept
{
	prepare();
	for (unsigned operation = 0; operation < operations().size(); ++operation)
		for (unsigned profile = 0; profile < 3; ++profile)
			if (operations()[operation].profiles & (1u << profile))
				checkWindow(operation, profile);
	// uint32_t is unsigned long in this ARM toolchain, unsigned int on the
	// host. The explicit array type avoids initializer-list type deduction.
	for (auto id : std::array<std::uint32_t, 3>{128u, 0x10000u, UINT32_MAX}) {
		qualification::check(namedVisitor(id) == 0);
		qualification::check(lambdaVisitor(id) == 0);
		qualification::check(nativeAs(id) == 0);
		qualification::check(encoded(id) == 0);
	}
}
} // namespace mcu
#endif

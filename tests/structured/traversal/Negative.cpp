/* Borrowing, typed positions and explicit conversion diagnostics. MIT. */
#include "Fixture.hpp"
#include <utility>
using namespace fixture;

int main()
{
#if CASE <= 36
#if (CASE - 1) % 6 == 0
	const auto& table = localFields;
#elif (CASE - 1) % 6 == 1
	const auto& table = localCommands;
#elif (CASE - 1) % 6 == 2
	const auto& table = localServices;
#elif (CASE - 1) % 6 == 3
	const auto& table = fields;
#elif (CASE - 1) % 6 == 4
	const auto& table = commands;
#else
	const auto& table = services;
#endif
#if CASE <= 6
	(void)std::move(table).get<0>();
#elif CASE <= 12
	(void)std::move(table).begin();
#elif CASE <= 18
	(void)std::move(table).end();
#elif CASE <= 24
	(void)std::move(table)[0];
#elif CASE <= 30
	std::move(table).forEach([](auto&&...) {
	});
#else
	(void)std::move(table).visit(0, [](const auto&) {
	});
#endif
#elif CASE == 37
	(void)localFields.get<10>();
#elif CASE == 38
	(void)localCommands.get<3>();
#elif CASE == 39
	(void)localServices.get<4>();
#elif CASE == 40
	(void)telemetry::detail::CatalogAccess::get<telemetry::detail::makeId<3, 0>()>(fields);
#elif CASE == 41
	(void)telemetry::detail::CatalogAccess::get<telemetry::detail::makeId<3, 0>()>(commands);
#elif CASE == 42
	(void)telemetry::detail::CatalogAccess::get<telemetry::detail::makeId<3, 0>()>(services);
#elif CASE == 43
	(void)telemetry::detail::CatalogAccess::get<Position::U16>(fields);
#elif CASE == 44
	(void)fields.visit(Position::U16, [](const auto&) {
	});
#elif CASE == 45
	(void)fields.visit<std::uint16_t>(65537, [](const auto&) {
	});
#elif CASE == 46
	(void)fields.readAs<float, std::uint16_t>(65537);
#elif CASE == 47
	(void)fields.writeAs<std::uint16_t>(65537, 1);
#elif CASE == 48
	volatile float value = 1;
	(void)fields.writeAs(0, value);
#elif CASE == 49
	(void)localFields.readAs<Config, 1>();
#elif CASE == 50
	(void)localFields.writeAs<1>(Config{});
#elif CASE == 51
	(void)localFields.readAs<float, 7>();
#elif CASE == 52
	(void)localFields.get<UINT64_C(0x100000001)>();
#elif CASE == 53
	(void)localFields.get<-1>();
#elif CASE == 54
	(void)std::move(localFields).readAs<float>(1);
#elif CASE == 55
	(void)std::move(fields).readAs<float>(1);
#elif CASE == 56
	(void)std::move(localFields).writeAs(1, 1);
#elif CASE == 57
	(void)std::move(fields).writeAs(1, 1);
#elif CASE == 58
	(void)std::move(localFields).readAs<float, 1>();
#elif CASE == 59
	(void)std::move(fields).readAs<float, 1>();
#elif CASE == 60
	(void)std::move(localFields).writeAs<1>(1);
#elif CASE == 61
	(void)std::move(fields).writeAs<1>(1);
#elif CASE == 62
	(void)telemetry::detail::CatalogAccess::get<UINT64_C(0x100000001)>(fields);
#elif CASE == 63
	(void)telemetry::detail::CatalogAccess::get<telemetry::detail::makeId<0, 3>()>(commands);
#elif CASE == 64
	(void)telemetry::detail::CatalogAccess::get<telemetry::detail::makeId<0, 4>()>(services);
#else
#error Unknown negative case
#endif
}

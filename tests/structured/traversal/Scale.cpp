/* Incremental object sections for 32/128 positional native visitor rows. MIT. */
#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
namespace ts = telemetry;
static std::uint32_t value;

std::uint32_t read() noexcept
{
	return value;
}

template<std::size_t... I>
constexpr auto make(std::index_sequence<I...>)
{
	return ts::FieldTable{((void)I, ts::field<&read>("Value"))...};
}

static constexpr auto table = make(std::make_index_sequence<ROWS>{});
static constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("group", table)};

extern "C" const char* selected_name(std::uint32_t id)
{
#if WITH_VISITOR
	const char* result = nullptr;
	(void)catalogs.visit(id, [&](const auto& endpoint) {
		result = endpoint.name();
	});
	return result;
#else
	const auto* entry = catalogs.index().find(id);
	return entry ? entry->name : nullptr;
#endif
}

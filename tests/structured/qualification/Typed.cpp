/* Native consumer: heterogeneous definitions and reused visitor types.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT. */
#include "Fixture.hpp"

namespace qualification {
// Named visitor counts immutable names while typed metadata traversal borrows definitions.
// API: operator().
struct NameVisitor {
	const char** result;

	template<class Endpoint>
	void operator()(const Endpoint& endpoint) const noexcept
	{
		*result = endpoint.name();
	}
};

extern "C" void consumer_typed() noexcept
{
	using namespace fixture;
	// The tree's first group holds the local table's definitions again.
	check(std::is_same_v<std::remove_cvref_t<decltype(ts::detail::CatalogAccess::get<0>(fields))>,
	                     std::remove_cvref_t<decltype(localFields.get<0>())>>);
	check(fields.readAs<std::uint64_t>(4u) == UINT64_MAX);
	check(fields.readAs<std::int64_t>(3u) == INT64_MIN);
	check(fields.writeAs(1u, 123.75) == W::Applied && device.integer == 123);
	check(fields.readAs<double>(1u) == 123.0);
	check(!fields.readAs<Config>(1u));
	check(!fields.readAs<double>(0x100000000ULL));

	const char* name = nullptr;
	const auto reads = device.reads;
	for (unsigned id : {8u, 0x20008u}) {
		check(fields.visit(id, NameVisitor{&name}));
		// Literal pooling across TUs is not a pointer-identity guarantee.
		check(std::string_view{name} == localFields.get<8>().name());
	}
	check(device.reads == reads); // Visiting a Big definition does not make Big.
	check(!fields.visit(0x10000u, NameVisitor{&name}));
	check(!fields.visit(UINT64_MAX, NameVisitor{&name}));

	unsigned visited = 0;
	fields.forEach([&]<std::size_t G, std::size_t I>(std::string_view group, const auto& endpoint) {
		check((G == 0 || G == 2) && I < localFields.size());
		check(!group.empty() && endpoint.name()[0] != '\0');
		++visited;
	});
	check(visited == 2 * localFields.size());
	check(fields.empty() == false && noFields.empty() && noFieldCatalogs.empty());

	Config request{77, false};
	check(ts::detail::CatalogAccess::call<0>(commands, request) == C::Executed);
	check(device.config.code == 77 && !device.config.enabled);
	const auto response = ts::detail::CatalogAccess::call<0>(services, request);
	check(response.status() == ts::ServiceStatus::Ok && response.value().code == 77 &&
	      !response.value().enabled);
	check(localFields.read<7>()->code == 77);
	check(!fields.readAs<Config>(9u));
	slot.bind(device);
	check(fields.readAs<Config>(9u)->code == 77);
	check(fields.writeAs(9u, Config{88, true}) == W::Applied);
	slot.reset();
	check(!fields.readAs<Config>(9u));

	const auto view = modelView();
	check(view.fieldTypeId(7u) == view.commandTypeId(0u));
	const auto pair = view.serviceTypeIds(0u);
	check(pair && pair->requestTypeId == *view.fieldTypeId(7u) &&
	      pair->responseTypeId == pair->requestTypeId);
}
} // namespace qualification

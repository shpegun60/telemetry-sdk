/* Copyable typed ReadMany usage with an installed generated SDK. MIT. */
#include "Fixture.hpp"

using namespace static_many_test;

int main()
{
	// The test fixture supplies an admitted client and a real Encoded peer.
	// An application supplies its own owner loop/transport once, in the same way
	// as for existing client.read(), write(), command() and service().
	Harness<api::client::CodecPolicy, 2, 128, 128> connection;
	auto& client = connection.client;
	auto& backend = connection.backend;
	const auto capability = connection.capability;

	const auto selected = rm::fields(api::left::F05, api::mixed::U64);
	const auto bound = rm::bindReadMany(client, backend, capability, selected);
	if (!bound)
		return 1;

	bool received = false;
	const auto submitted = rm::readMany(client, bound.binding, [&](const auto& result) noexcept {
		if (!result.operation.succeeded() || result.valueStatus != rm::StaticValueStatus::Ok)
			return;
		const auto& first = result.template get<0>();
		const auto& second = result.template get<1>();
		if (!first.hasValue() || !second.hasValue())
			return;
		CHECK(first.value() == native::word(5));
		CHECK(second.value() == UINT64_MAX);
		received = true;
	});
	if (!submitted)
		return 2;

	// Existing transport work, rather than any second ReadMany execution queue.
	connection.send(connection.offered());
	CHECK(received && client.pending() == 0);
	connection.stop();
	std::printf("Static ReadMany example checks=%u failures=0\n", checks);
	return 0;
}

/* Successful typed mixed-batch user code over a real Encoded test connection. MIT. */
#include "Fixture.hpp"
using namespace static_batch_test;

int main()
{
	// Connection/transport setup is shared with ordinary typed operations.
	Harness<api::client::CodecPolicy, 2, 512> connection;
	auto& client = connection.client;
	auto& backend = connection.backend;
	Data arguments = input();
	const auto request =
	    bt::items(bt::write(api::control::Config, arguments), bt::read(api::control::Config),
	              bt::command(api::control::Configure, arguments),
	              bt::service(api::control::Echo, arguments));
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	const auto bound = bt::bindBatch(client, backend, connection.capability, request);
	if (!bound)
		return 1;
	bool completed = false;
	const auto submitted =
	    bt::batch(client, bound.binding, request, storage, [&](const auto& result) noexcept {
		    if (!result.operation.succeeded() || result.valueStatus != bt::StaticValueStatus::Ok)
			    return;
		    const auto written = result.template get<0>();
		    const auto readback = result.template get<1>();
		    const auto command = result.template get<2>();
		    const auto service = result.template get<3>();
		    if (!written || !readback || !command || !service)
			    return;
		    CHECK(written->operation.succeeded() && command->operation.executed());
		    CHECK(readback->hasValue() && service->hasValue());
		    exact(readback->value(), 1000); // Device-normalized value, not a local echo.
		    exact(service->value(), 1111);
		    completed = true;
	    });
	if (!submitted)
		return 2;
	connection.send(connection.offered()); // Existing owner/transport loop.
	CHECK(completed && !storage.busy());
	connection.stop();
	std::printf("Static Batch example checks=%u failures=0\n", checks);
	return 0;
}

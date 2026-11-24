/* Discovery -> descriptor -> Bind -> values -> controls over bounded peers. MIT. */
#include "FakeTransport.hpp"
#include <cstdio>

using namespace fixture;

int main()
{
	example::Transport transport;
	const auto first = transport.open();
	const auto second = transport.open();
	assert(first && second && !transport.open()); // Even two Unbound peers exhaust capacity.
	std::array<std::byte, 8192> output{};
	std::array<std::byte, 13> read{};
	read[0] = std::byte{3};
	const auto descriptorReply = transport.resource(*first, read, output);
	assert(descriptorReply && descriptorReply->status == resource::Status::Ok);
	assert(descriptorReply->written == packed.size() + 12);
	assert(std::equal(packed.begin(), packed.end(), output.begin() + 12));
	put(read, 1, std::uint32_t{1});
	assert(!transport.resource(*first, read, output)); // Values blocked before Bind.
	const auto request = packet(Op::Service, 0, Config{777, true});
	assert(transport.exchange(*first, request, output).dispatch == D::NotReady &&
	       callbackCount() == 0);
	assert(transport.bind(*first, handshake(descriptor.fingerprint() ^ 1), output).dispatch ==
	       D::NotReady);
	assert(transport.bind(*first, handshake(), output).dispatch == D::Ok);
	assert(!transport.resource(*second, read, output));
	const auto valuesReply = transport.resource(*first, read, output);
	assert(valuesReply && valuesReply->status == resource::Status::Ok && device.reads == 3);
	assert(output[12] == std::byte{'T'} && output[13] == std::byte{'V'});

	const auto agreements = transport.agreements;
	for (unsigned i = 0; i < 50; ++i) {
		assert(transport.exchange(*first, request, output).dispatch == D::Ok);
		const auto field = packet(Op::FieldWrite, 0, Config{i, true});
		assert(transport.exchange(*first, field, output).dispatch == D::Ok &&
		       device.current.value == i);
		const auto command = packet(Op::Command, 0, Config{i + 1, false});
		assert(transport.exchange(*first, command, output).dispatch == D::Ok &&
		       device.current.value == i + 1);
	}
	assert(transport.agreements == agreements && device.services == 50 && device.commands == 50 &&
	       device.writes == 50);
	assert(transport.bind(*second, handshake(), output).dispatch == D::Ok);
	assert(transport.exchange(*second, request, output).dispatch ==
	       D::Ok); // Same correlation ID on another peer.

	const auto reset = emptyPacket(Op::Command, 1);
	assert(transport.queue(*first, reset) && transport.queue(*first, reset));
	assert(!transport.queue(*first, reset));
	assert(!transport.canCloseDuringHandler(*first));
	assert(transport.close(*first));
	const auto reused = transport.open();
	assert(reused && reused->position == first->position);
	assert(transport.exchange(*reused, request, output).dispatch == D::NotReady);
	assert(transport.bind(*reused, handshake(), output).dispatch == D::Ok);
	const auto before = callbackCount();
	transport.drain();
	assert(callbackCount() == before); // Close cleared queued old commands.
	assert(transport.exchange(*first, request, output).written ==
	       0); // Late old connection cannot reach new Ready.
	assert(transport.exchange(*second, request, output).dispatch == D::Ok);
	transport.reboot(); // Model replacement follows the same close/queue boundary.
	const auto fresh = transport.open();
	assert(fresh);
	assert(transport.exchange(*fresh, request, output).dispatch == D::NotReady);
	assert(transport.exchange(*second, request, output).written == 0);

	// A real positional schema change, not just a fabricated wrong hash.
	static constexpr ts::Model shiftedModel{ts::group(
	    "device", readOnlyField, configField, configureCommand, resetCommand, optionalCommand,
	    bigCommand, echoService, pingService, optionalService, largeService)};
	static constexpr auto shiftedView = shiftedModel.view();
	static constexpr ::telemetry::descriptor::Descriptor shiftedDescriptor{shiftedModel};
	static_assert(shiftedDescriptor.fingerprint() != descriptor.fingerprint());
	rs::Binding changed;
	const auto callbacks = callbackCount();
	assert(rs::Bind::process(changed, shiftedView, shiftedDescriptor.fingerprint(), handshake(),
	                         output)
	           .dispatch == D::NotReady);
	std::array<std::byte, model.maxScratch()> storage{};
	ts::Workspace workspace{storage};
	assert(rs::Exchange::process(changed, request, output, workspace).dispatch == D::NotReady);
	assert(callbackCount() == callbacks && !changed.ready());

	example::Pending pending;
	const auto zero = pending.add(Op::Command, 1);
	assert(zero && *zero == 0);
	pending.next = UINT32_MAX;
	const auto last = pending.add(Op::Service, 0);
	assert(last && *last == UINT32_MAX);
	assert(!pending.add(Op::Service, 0)); // Capacity includes timed-out requests.
	assert(!pending.complete(0, Op::Service, 1) && !pending.complete(0, Op::Command, 2));
	assert(pending.complete(UINT32_MAX, Op::Service, 0));
	const auto wrapped = pending.add(Op::FieldWrite, 0);
	assert(wrapped && *wrapped == 1); // Zero still outstanding.
	assert(pending.complete(0, Op::Command, 1) && !pending.complete(0, Op::Command, 1));
	pending.reset();
	assert(pending.add(Op::Service, 0));
	std::puts("Stage 11 bounded transport and correlation checks passed");
}

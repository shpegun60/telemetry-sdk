/* Optional Bind/Exchange packet, peer-state, status, lifetime and alias checks. MIT. */
#include "Fixture.hpp"
#include <cstdio>
#include <fstream>
#include <vector>

using namespace fixture;
using Bytes = std::vector<std::byte>;
static unsigned checks = 0;

void responseIs(Input bytes, const rs::PacketResult& result, D status, unsigned endpoint,
                Op operation, std::uint32_t id, std::uint32_t payload = 0,
                std::uint32_t requestId = 0x10203040)
{
	assert(result.dispatch == status && result.written == 24 + payload);
	assert(bytes[0] == std::byte{'T'} && bytes[1] == std::byte{'S'} && bytes[2] == std::byte{'R'} &&
	       bytes[3] == std::byte{'P'});
	assert(bytes[4] == std::byte{3} && bytes[5] == std::byte{0} && bytes[6] == std::byte{0} &&
	       bytes[7] == std::byte{0});
	assert(get32(bytes, 8) == requestId && get32(bytes, 12) == id && get32(bytes, 16) == payload);
	assert(bytes[20] == static_cast<std::byte>(operation) &&
	       bytes[21] == static_cast<std::byte>(status));
	assert(bytes[22] == std::byte(endpoint) && bytes[23] == std::byte{0});
	++checks;
}

void binds()
{
	rs::Binding a, b;
	const auto valid = handshake();
	for (unsigned length = 0; length <= 18; ++length) {
		for (unsigned capacity = 0; capacity <= 10; ++capacity) {
			ready(a); // Every failure, including a short response, clears old Ready.
			Bytes input(18);
			std::copy(valid.begin(), valid.end(), input.begin());
			Bytes output(capacity + 2, std::byte{0xcc});
			const auto result =
			    rs::Bind::process(a, view, descriptor.fingerprint(), Input{input}.first(length),
			                      Output{output}.subspan(1, capacity));
			const bool enough = capacity >= 8;
			const bool ok = enough && length == 16;
			assert(result.dispatch == (!enough ? D::BufferTooSmall
			                           : ok    ? D::Ok
			                                   : D::InvalidRequest));
			assert(result.written == (enough ? 8 : 0) && a.ready() == ok && !b.ready());
			if (enough) {
				assert(output[1] == std::byte{'T'} && output[4] == std::byte{'A'});
				assert(output[5] == (ok ? std::byte{0} : std::byte{3}));
				assert(output[6] == std::byte{0} && output[7] == std::byte{0} &&
				       output[8] == std::byte{0});
			}
			assert(output.front() == std::byte{0xcc});
			assert(std::all_of(output.begin() + 1 + result.written, output.end(), [](auto v) {
				return v == std::byte{0xcc};
			}));
			++checks;
		}
	}
	for (const auto offset : {0, 4, 6, 8, 15}) {
		auto input = valid;
		input[offset] ^= std::byte{1};
		std::array<std::byte, 8> output{};
		ready(a);
		const auto result = rs::Bind::process(a, view, descriptor.fingerprint(), input, output);
		assert(!a.ready() && result.written == 8);
		assert(output[4] == std::byte(offset == 0 ? 3 : offset < 8 ? 2 : 1));
		++checks;
	}
	// Both directions of partial overlap, plus the same buffer.
	for (unsigned source = 0; source <= 12; ++source) {
		for (unsigned destination = 0; destination <= 12; ++destination) {
			std::array<std::byte, 32> memory{};
			std::copy(valid.begin(), valid.end(), memory.begin() + source);
			const auto result = rs::Bind::process(a, view, descriptor.fingerprint(),
			                                      Input{memory}.subspan(source, 16),
			                                      Output{memory}.subspan(destination, 8));
			assert(result.dispatch == D::Ok && a.ready() &&
			       memory[destination + 4] == std::byte{0});
			++checks;
		}
	}
	a.reset();
	assert(!a.ready());
}

void statuses()
{
	device = Device{};
	slot.reset();
	rs::Binding peer;
	ready(peer);
	std::array<std::byte, model.maxScratch()> memory{};
	ts::Workspace workspace{memory};
	std::array<std::byte, 64> output{};
	const Config value{0x12345678, true};
	auto request = packet(Op::FieldWrite, 0, value);
	const std::array<W, 6> writes{W::Applied,      W::NotFound, W::ReadOnly,
	                              W::InvalidValue, W::Busy,     W::Unavailable};
	for (unsigned i = 0; i < writes.size(); ++i) {
		device.writeStatus = writes[i];
		const auto before = callbackCount();
		const auto result = rs::Exchange::process(peer, request, output, workspace);
		responseIs(output, result, D::Ok, i, Op::FieldWrite, 0);
		assert(callbackCount() == before + 1 && device.current.value == value.value);
	}
	request = packet(Op::Command, 0, value);
	const std::array<C, 8> commands{
	    C::Executed,     C::Accepted, C::NotFound, C::Unavailable, C::ArgumentCountMismatch,
	    C::InvalidValue, C::Busy,     C::Failed};
	for (unsigned i = 0; i < commands.size(); ++i) {
		device.commandStatus = commands[i];
		responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::Ok, i,
		           Op::Command, 0);
	}
	request = packet(Op::Service, 0, value);
	const std::array<S, 5> services{S::Ok, S::InvalidArgument, S::Unavailable, S::Busy, S::Failed};
	for (unsigned i = 0; i < services.size(); ++i) {
		device.serviceStatus = services[i];
		output.fill(std::byte{0xcc});
		const auto result = rs::Exchange::process(peer, request, output, workspace);
		responseIs(output, result, D::Ok, i, Op::Service, 0, i == 0 ? 5 : 0);
		assert(std::all_of(output.begin() + result.written, output.end(), [](auto v) {
			return v == std::byte{0xcc};
		}));
		const auto ping = emptyPacket(Op::Service, 1);
		responseIs(output, rs::Exchange::process(peer, ping, output, workspace), D::Ok, i,
		           Op::Service, 1);
	}
	device = Device{};
	for (const auto operation : {Op::FieldWrite, Op::Command, Op::Service}) {
		request = packet(operation, 2, value);
		const auto before = callbackCount();
		responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::Unavailable,
		           0, operation, 2);
		request.back() = std::byte{2};
		responseIs(output, rs::Exchange::process(peer, request, output, workspace),
		           D::InvalidPayload, 0, operation, 2);
		assert(callbackCount() == before);
		request.back() = std::byte{1};
		slot.bind(device);
		const auto result = rs::Exchange::process(peer, request, output, workspace);
		responseIs(output, result, D::Ok, 0, operation, 2, operation == Op::Service ? 5 : 0);
		assert(callbackCount() == before + 1);
		slot.reset();
	}
	// Unknown user statuses must never become an undocumented wire byte.
	device.writeStatus = static_cast<W>(255);
	request = packet(Op::FieldWrite, 0, value);
	responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::InternalError, 0,
	           Op::FieldWrite, 0);
	device.commandStatus = static_cast<C>(255);
	request = packet(Op::Command, 0, value);
	responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::InternalError, 0,
	           Op::Command, 0);
	device = Device{};

	// ReadOnly capability wins over field payload decoding, including its length.
	request = packet(Op::FieldWrite, 1, value);
	request.back() = std::byte{255};
	responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::Ok, 2,
	           Op::FieldWrite, 1);
	auto empty = emptyPacket(Op::FieldWrite, 1);
	responseIs(output, rs::Exchange::process(peer, empty, output, workspace), D::Ok, 2,
	           Op::FieldWrite, 1);
	assert(device.writes == 0 && workspace.used() == 0);

	// Correlation is echoed, not stored or deduplicated, including zero/wrap.
	empty = emptyPacket(Op::Command, 1);
	for (const auto id : {std::uint32_t{0}, UINT32_MAX, std::uint32_t{0}, std::uint32_t{1}}) {
		put(empty, 8, id);
		responseIs(output, rs::Exchange::process(peer, empty, output, workspace), D::Ok, 0,
		           Op::Command, 1, 0, id);
	}
	assert(device.commands == 4);
}

void malformed()
{
	rs::Binding peer;
	ready(peer);
	std::array<std::byte, model.maxScratch()> memory{};
	ts::Workspace workspace{memory};
	const auto valid = packet(Op::Service, 0, Config{77, false});
	for (unsigned inputSize = 0; inputSize <= 31; ++inputSize) {
		for (unsigned capacity = 0; capacity <= 32; ++capacity) {
			for (unsigned alignment = 0; alignment < 4; ++alignment) {
				device = Device{};
				Bytes input(40);
				std::copy(valid.begin(), valid.end(), input.begin() + alignment);
				Bytes output(capacity + 8, std::byte{0xcd});
				const auto result =
				    rs::Exchange::process(peer, Input{input}.subspan(alignment, inputSize),
				                          Output{output}.subspan(3, capacity), workspace);
				const D wanted = capacity < 24               ? D::BufferTooSmall
				                 : inputSize < 24            ? D::InvalidRequest
				                 : inputSize != valid.size() ? D::InvalidPayload
				                 : capacity < 29             ? D::BufferTooSmall
				                                             : D::Ok;
				const unsigned written = capacity < 24 || inputSize < 24 ? 0
				                         : wanted == D::Ok               ? 29
				                                                         : 24;
				assert(result.dispatch == wanted && result.written == written);
				assert(device.services == unsigned(wanted == D::Ok));
				assert(workspace.used() == 0);
				assert(std::all_of(output.begin(), output.begin() + 3, [](auto v) {
					return v == std::byte{0xcd};
				}));
				assert(std::all_of(output.begin() + 3 + written, output.end(), [](auto v) {
					return v == std::byte{0xcd};
				}));
				++checks;
			}
		}
	}
	for (unsigned offset = 0; offset < 24; ++offset) {
		auto request = valid;
		request[offset] = std::byte{255};
		std::array<std::byte, 40> output{};
		device = Device{};
		const auto result = rs::Exchange::process(peer, request, output, workspace);
		if (offset < 4)
			assert(result.dispatch == D::InvalidRequest && result.written == 0);
		else if (offset < 8)
			assert(result.dispatch == D::UnsupportedVersion);
		else if (offset < 12)
			assert(result.dispatch == D::Ok && device.services == 1);
		else if (offset < 16)
			assert(result.dispatch == D::NotFound);
		else if (offset < 20)
			assert(result.dispatch == D::InvalidPayload);
		else
			assert(result.dispatch == D::InvalidRequest);
		assert(offset >= 8 && offset < 12 ? device.services == 1 : device.services == 0);
		++checks;
	}
	for (const auto id : {UINT32_MAX, std::uint32_t{65536}, std::uint32_t{4}}) {
		for (const auto op : {Op::FieldWrite, Op::Command, Op::Service}) {
			auto request = packet(op, id, Config{});
			std::array<std::byte, 40> output{};
			responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::NotFound,
			           0, op, id);
			peer.reset();
			responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::NotReady,
			           0, op, id);
			ready(peer);
		}
	}
}

void aliases()
{
	device = Device{};
	rs::Binding peer;
	ready(peer);
	std::array<std::byte, model.maxScratch()> memory{};
	ts::Workspace workspace{memory};
	const auto valid = packet(Op::Service, 0, Config{0x12345678, true});
	for (unsigned source = 0; source <= 35; source += 5) {
		for (unsigned destination = 0; destination <= 35; destination += 5) {
			std::array<std::byte, 80> bytes{};
			std::copy(valid.begin(), valid.end(), bytes.begin() + source);
			const auto result =
			    rs::Exchange::process(peer, Input{bytes}.subspan(source, valid.size()),
			                          Output{bytes}.subspan(destination, 29), workspace);
			responseIs(Input{bytes}.subspan(destination), result, D::Ok, 0, Op::Service, 0, 5);
			assert(std::equal(valid.begin() + 24, valid.end(), bytes.begin() + destination + 24));
		}
	}
	// Small local endpoints ignore even a Workspace aliasing the wire buffers.
	if (view.services.catalogs()[0].entries[0].scratchBytes == 0) {
		auto request = valid;
		ts::Workspace overlapping{request};
		const auto result = rs::Exchange::process(peer, request, request, overlapping);
		responseIs(request, result, D::Ok, 0, Op::Service, 0, 5);
		assert(overlapping.used() == 0);
	}
}

void scratch()
{
	device = Device{};
	rs::Binding peer;
	ready(peer);
	auto request = packet(Op::Service, 3, Config{1234, true});
	std::array<std::byte, 24 + ts::wireSize<Big> + 8> output{};
	std::array<std::byte, model.maxScratch() + 128> memory{};
	const auto required = view.services.catalogs()[0].entries[3].scratchBytes;
	for (unsigned alignment = 0; alignment < 8; ++alignment) {
		for (int delta = -1; delta <= 1; ++delta) {
			ts::Workspace workspace{Output{memory}.subspan(alignment, required + delta)};
			const auto before = device.services;
			output.fill(std::byte{0xcc});
			const auto result = rs::Exchange::process(peer, request, output, workspace);
			responseIs(output, result, delta < 0 ? D::WorkspaceTooSmall : D::Ok, 0, Op::Service, 3,
			           delta < 0 ? 0 : ts::wireSize<Big>);
			assert(device.services == before + unsigned(delta >= 0) && workspace.used() == 0);
			if (delta >= 0)
				assert(get32(output, 24) == 1234 && get32(output, 24 + 4092) == 0);
			assert(std::all_of(output.begin() + result.written, output.end(), [](auto v) {
				return v == std::byte{0xcc};
			}));
		}
	}
	ts::Workspace workspace{memory};
	auto lease = workspace.reserve<std::array<std::byte, 128>>();
	auto* sentinel = lease.constructFrom([] {
		std::array<std::byte, 128> a;
		a.fill(std::byte{0xab});
		return a;
	});
	assert(sentinel != nullptr);
	const auto used = workspace.used();
	const auto before = device.services;
	responseIs(output,
	           rs::Exchange::process(peer, request,
	                                 Output{output}.first(24 + ts::wireSize<Big> - 1), workspace),
	           D::BufferTooSmall, 0, Op::Service, 3);
	assert(device.services == before && workspace.used() == used);
	responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::Ok, 0,
	           Op::Service, 3, ts::wireSize<Big>);
	assert(workspace.used() == used && std::all_of(sentinel->begin(), sentinel->end(), [](auto v) {
		       return v == std::byte{0xab};
	       }));

	// Never overwrite an active scratch object merely to report the overlap.
	auto result = rs::Exchange::process(peer, request, memory, workspace);
	assert(result.dispatch == D::InvalidPayload && result.written == 0 && workspace.used() == used);
	assert(std::all_of(sentinel->begin(), sentinel->end(), [](auto v) {
		return v == std::byte{0xab};
	}));
	++checks;
	// The header alone overlapping scratch is sufficient; no payload overlap needed.
	std::copy(request.begin(), request.end(), memory.begin() + 128);
	responseIs(
	    output,
	    rs::Exchange::process(peer, Input{memory}.subspan(128, request.size()), output, workspace),
	    D::InvalidPayload, 0, Op::Service, 3);

	for (const auto op : {Op::FieldWrite, Op::Command}) {
		std::array<std::byte, model.maxScratch()> otherMemory{};
		Big value{};
		value.words.back() = 456;
		const auto largeRequest = packet(op, 3, value);
		ts::Workspace empty{Output{otherMemory}.first(sizeof(Big) - 1)};
		const auto count = callbackCount();
		responseIs(output, rs::Exchange::process(peer, largeRequest, output, empty),
		           D::WorkspaceTooSmall, 0, op, 3);
		assert(callbackCount() == count && empty.used() == 0);
		ts::Workspace enough{otherMemory};
		responseIs(output, rs::Exchange::process(peer, largeRequest, output, enough), D::Ok, 0, op,
		           3);
		assert(callbackCount() == count + 1 && device.current.value == 456 && enough.used() == 0);
	}
}

ts::EncodedCallResult invalidResult(const void*, const std::byte*, std::byte*,
                                    ts::Workspace&) noexcept
{
	return {ts::DispatchStatus::Ok, static_cast<S>(255), 0};
}

void damagedAdapter()
{
	// Inject a broken custom erased handler, while retaining the native shape.
	auto entry = view.services.catalogs()[0].entries[0];
	entry.invoke = invalidResult;
	const ts::ServiceCatalog catalog{"broken", &entry, 1};
	auto broken = view;
	broken.services = ts::ServiceIndex{&catalog, 1};
	rs::Binding peer;
	std::array<std::byte, 8> bound{};
	assert(rs::Bind::process(peer, broken, descriptor.fingerprint(), handshake(), bound).dispatch ==
	       D::Ok);
	std::array<std::byte, model.maxScratch()> memory{};
	ts::Workspace workspace{memory};
	std::array<std::byte, 29> output{};
	const auto request = packet(Op::Service, 0, Config{});
	responseIs(output, rs::Exchange::process(peer, request, output, workspace), D::InternalError, 0,
	           Op::Service, 0);
	peer.reset();
}

int main(int argc, char** argv)
{
	binds();
	statuses();
	malformed();
	aliases();
	scratch();
	damagedAdapter();
	if (argc == 2) {
		const auto bind = handshake(0x0102030405060708ULL);
		const auto request = packet(Op::Service, 0, Config{0x12345678, true});
		std::array<std::byte, model.maxScratch()> memory{};
		ts::Workspace workspace{memory};
		rs::Binding peer;
		ready(peer);
		std::array<std::byte, 29> response{};
		assert(rs::Exchange::process(peer, request, response, workspace).written ==
		       response.size());
		std::ofstream file{argv[1], std::ios::binary};
		for (const auto bytes : {Input{bind}, Input{request}, Input{response}})
			file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		assert(file.good());
	}
	std::printf("Stage 11 packet checks: %u passed\n", checks);
}

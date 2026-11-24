/*
 * @file Server.cpp
 * @brief Real bounded Model reads, global preflight and opt-in Encoded server checks.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include <telemetry/Telemetry.hpp>
#include <telemetry/encoded/ReadMany.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace ts = telemetry;
namespace ew = ts::protocol::encoded;
namespace rm = ew::read_many;
namespace server = ts::integration::encoded;
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
static unsigned checks = 0;

static void check(bool condition, const char* message)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "ReadMany server check=%u failed: %s\n", checks, message);
		std::abort();
	}
}

static std::uint64_t load(Input input)
{
	std::uint64_t result = 0;
	for (std::size_t i = 0; i < input.size(); ++i)
		result |= std::uint64_t{std::to_integer<unsigned>(input[i])} << (8 * i);
	return result;
}

struct ScalarOwner {
	unsigned calls = 0;

	template<std::size_t I>
	std::uint64_t read() noexcept
	{
		++calls;
		return (std::uint64_t{I} << 32) | 0x11223344u;
	}
};

static ScalarOwner scalar;

template<std::size_t Offset, std::size_t... I>
static constexpr auto fields(std::index_sequence<I...>)
{
	return ts::FieldTable{ts::field<&ScalarOwner::template read<Offset + I>>("Value", scalar)...};
}

static constexpr auto left = fields<0>(std::make_index_sequence<16>{});
static constexpr auto right = fields<16>(std::make_index_sequence<16>{});

// Names in one group are unique in a tree: Value0..Value15.
static constexpr std::array<std::array<char, 8>, 16> valueNames = [] {
	std::array<std::array<char, 8>, 16> names{};
	for (std::size_t i = 0; i < names.size(); ++i)
		names[i] = {'V',
		            'a',
		            'l',
		            'u',
		            'e',
		            char(i < 10 ? '0' + i : '1'),
		            char(i < 10 ? '\0' : '0' + i - 10),
		            '\0'};
	return names;
}();

template<std::size_t Offset, std::size_t... I>
static constexpr auto branch(const char* name, std::index_sequence<I...>)
{
	return ts::group(
	    name, ts::field<&ScalarOwner::template read<Offset + I>>(valueNames[I].data(), scalar)...);
}

// The same 32 getters as two Fields-only groups of a tree.
static constexpr ts::Model model{branch<0>("left", std::make_index_sequence<16>{}),
                                 branch<16>("right", std::make_index_sequence<16>{})};

enum class Mode : std::int16_t {
	Reverse = -7,
	Forward = 12
};

struct OneWord {
	std::uint32_t value;
};

struct MixedOwner {
	unsigned calls = 0;

	bool enabled() noexcept
	{
		++calls;
		return true;
	}

	std::uint64_t wide() noexcept
	{
		++calls;
		return UINT64_MAX;
	}

	std::int64_t signedWide() noexcept
	{
		++calls;
		return INT64_MIN;
	}

	float real() noexcept
	{
		++calls;
		return std::bit_cast<float>(0xffc12345u);
	}

	Mode mode() noexcept
	{
		++calls;
		return static_cast<Mode>(-123);
	}

	OneWord record() noexcept
	{
		++calls;
		return {17};
	}

	std::array<std::uint8_t, 4> array() noexcept
	{
		++calls;
		return {1, 2, 3, 4};
	}
};

static MixedOwner mixed;
static ts::OwnerSlot<MixedOwner> absent;
static constexpr ts::FieldTable mixedFields{ts::field<&MixedOwner::enabled>("Bool", mixed),
                                            ts::field<&MixedOwner::wide>("U64", mixed),
                                            ts::field<&MixedOwner::signedWide>("S64", mixed),
                                            ts::field<&MixedOwner::real>("F32", mixed),
                                            ts::field<&MixedOwner::mode>("Enum", mixed),
                                            ts::field<&MixedOwner::record>("Struct4", mixed),
                                            ts::field<&MixedOwner::array>("Array4", mixed),
                                            ts::field<&MixedOwner::wide>("Unavailable", absent)};
static constexpr ts::Model mixedModel{ts::group(
    "mixed", ts::field<&MixedOwner::enabled>("Bool", mixed),
    ts::field<&MixedOwner::wide>("U64", mixed), ts::field<&MixedOwner::signedWide>("S64", mixed),
    ts::field<&MixedOwner::real>("F32", mixed), ts::field<&MixedOwner::mode>("Enum", mixed),
    ts::field<&MixedOwner::record>("Struct4", mixed),
    ts::field<&MixedOwner::array>("Array4", mixed),
    ts::field<&MixedOwner::wide>("Unavailable", absent))};

struct alignas(16) AlignedScratch {
	std::array<std::byte, 16> bytes;
};

struct Adapted {
	std::array<ts::FieldEntry, 16> rows{};
	std::array<ts::FieldCatalog, 2> groups{};
	ts::ModelView view = model.view();

	Adapted(ts::FieldEntry::Read read, std::uint32_t scratch)
	{
		std::copy_n(left.data(), rows.size(), rows.begin());
		rows[0].read = read;
		rows[0].readContext = left.data();
		rows[0].readScratchBytes = scratch;
		groups = {ts::FieldCatalog{"left", rows.data(), 16},
		          ts::FieldCatalog{"right", right.data(), 16}};
		view.fields = ts::FieldIndex{groups.data(), 2};
	}

	static ts::EncodedReadResult partial(const void*, std::byte* output, ts::Workspace&) noexcept
	{
		++scalar.calls;
		std::fill_n(output, 8, std::byte{0xa5});
		return {ts::DispatchStatus::InternalError, 8};
	}

	static ts::EncodedReadResult shortRead(const void*, std::byte* output, ts::Workspace&) noexcept
	{
		++scalar.calls;
		output[0] = std::byte{0xa5};
		return {ts::DispatchStatus::Ok, 1};
	}

	static ts::EncodedReadResult scratch(const void*, std::byte* output,
	                                     ts::Workspace& workspace) noexcept
	{
		auto lease = workspace.reserve<AlignedScratch>();
		if (!lease.valid())
			return {ts::DispatchStatus::WorkspaceTooSmall, 0};
		++scalar.calls;
		for (std::size_t i = 0; i < 8; ++i)
			output[i] = std::byte((std::uint64_t{0x11223344u} >> (8 * i)) & 0xffu);
		return {ts::DispatchStatus::Ok, 8};
	}
};

static void coreChecks()
{
	alignas(16) std::array<std::byte, 128> scratch{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, 600> output{};
	std::array<ts::PackedId, 33> ids{};
	for (std::size_t i = 0; i < 32; ++i)
		ids[i] = std::uint32_t(i / 16) * 65536u + std::uint32_t(i % 16);
	for (std::size_t count :
	     {std::size_t{1}, std::size_t{8}, std::size_t{12}, std::size_t{16}, std::size_t{32}}) {
		scalar.calls = 0;
		output.fill(std::byte{0x5a});
		const auto result =
		    ts::readManyEncoded(model.view(), std::span{ids}.first(count), output, workspace);
		check(result.dispatch == ts::DispatchStatus::Ok, "core success");
		check(result.written == 2 + 16 * count && scalar.calls == count,
		      "full bound and one getter per ID");
		const auto reply =
		    rm::parseReply(Input{output}.first(result.written), std::span{ids}.first(count));
		check(bool(reply) && reply.count == count, "ordered canonical body");
		for (std::size_t i = 0; i < count; ++i) {
			check(reply.items[i].id == ids[i] && reply.items[i].status == rm::ItemStatus::Ok,
			      "cross-catalog IDs");
			check(reply.items[i].payload.size() == 8, "U64 extent");
			check(load(reply.items[i].payload) == ((std::uint64_t{i} << 32) | 0x11223344u),
			      "exact U64 bytes");
		}
		check(output[result.written] == std::byte{0x5a}, "writes only published body");
	}
	const auto refused = [&](std::span<const ts::PackedId> selected, Output target,
	                         ts::Workspace& ws, ts::DispatchStatus expected,
	                         ts::ModelView view = model.view()) {
		scalar.calls = 0;
		output.fill(std::byte{0x5a});
		std::array<std::byte, 600> before{};
		std::copy(target.begin(), target.end(), before.begin());
		const auto result = ts::readManyEncoded(view, selected, target, ws);
		check(result.dispatch == expected && result.written == 0, "global refusal status");
		check(scalar.calls == 0, "global failure invokes no getter");
		check(std::equal(target.begin(), target.end(), before.begin()),
		      "global failure leaves actual output unchanged");
	};
	refused({}, output, workspace, ts::DispatchStatus::InvalidPayload);
	refused(ids, output, workspace, ts::DispatchStatus::InvalidPayload);
	std::array<ts::PackedId, 2> duplicate{0, 0};
	refused(duplicate, output, workspace, ts::DispatchStatus::InvalidPayload);
	refused(std::span{ids}.first(32), Output{output}.first(513), workspace,
	        ts::DispatchStatus::BufferTooSmall);
	refused(std::span{ids}.first(1), std::as_writable_bytes(std::span{ids}), workspace,
	        ts::DispatchStatus::InvalidPayload);
	ts::Workspace idWorkspace{std::as_writable_bytes(std::span{ids})};
	refused(std::span{ids}.first(1), output, idWorkspace, ts::DispatchStatus::InvalidPayload);
	refused(std::span{ids}.first(1), scratch, workspace, ts::DispatchStatus::InvalidPayload);

	Adapted aligned{&Adapted::scratch, ts::scratchBytes<AlignedScratch>};
	ts::Workspace remaining{std::span{scratch}.subspan(1, 63)};
	{
		auto outer = remaining.reserve<std::array<std::byte, 32>>();
		check(outer.valid() && remaining.used() == 32, "live outer reservation");
		const auto done =
		    ts::readManyEncoded(aligned.view, std::span{ids}.first(1), output, remaining);
		check(done.dispatch == ts::DispatchStatus::Ok && remaining.used() == 32,
		      "aligned getter restores outer cursor");
	}
	check(remaining.used() == 0, "outer reservation released");
	ts::Workspace tooShort{std::span{scratch}.subspan(1, 62)};
	{
		auto outer = tooShort.reserve<std::array<std::byte, 32>>();
		check(outer.valid(), "short outer reservation");
		refused(std::span{ids}.first(1), output, tooShort, ts::DispatchStatus::WorkspaceTooSmall,
		        aligned.view);
		// A later item's scratch need must stop the earlier valid getter too.
		aligned.rows[1] = aligned.rows[0];
		aligned.rows[0] = left[0];
		refused(std::span{ids}.first(2), output, tooShort, ts::DispatchStatus::WorkspaceTooSmall,
		        aligned.view);
	}
	for (auto thunk : {&Adapted::partial, &Adapted::shortRead}) {
		Adapted broken{thunk, 0};
		scalar.calls = 0;
		const auto done =
		    ts::readManyEncoded(broken.view, std::span{ids}.first(2), output, workspace);
		check(done.dispatch == ts::DispatchStatus::Ok && scalar.calls == 2,
		      "partial failure does not suppress following getter");
		const auto parsed =
		    rm::parseReply(Input{output}.first(done.written), std::span{ids}.first(2));
		check(bool(parsed) && parsed.items[0].status == rm::ItemStatus::ReadError &&
		          parsed.items[0].payload.empty(),
		      "failed thunk publishes no bytes");
		check(parsed.items[1].status == rm::ItemStatus::Ok &&
		          load(parsed.items[1].payload) == 0x111223344ULL,
		      "following item remains intact");
		check(done.written == 26, "error omits eight value bytes");
	}

	const std::array<ts::PackedId, 9> mixedIds{0, 1, 2, 3, 4, 5, 6, 7, 0x10000u};
	mixed.calls = 0;
	const auto done = ts::readManyEncoded(mixedModel.view(), mixedIds, output, workspace);
	check(done.dispatch == ts::DispatchStatus::Ok && mixed.calls == 5,
	      "unsupported and unavailable items invoke no owning getter");
	const auto parsed = rm::parseReply(Input{output}.first(done.written), mixedIds);
	check(bool(parsed), "mixed complete body");
	check(load(parsed.items[0].payload) == 1, "canonical bool");
	check(load(parsed.items[1].payload) == UINT64_MAX, "exact maximum U64");
	check(load(parsed.items[2].payload) == 0x8000000000000000ULL, "exact minimum S64");
	check(load(parsed.items[3].payload) == 0xffc12345u, "NaN payload bits");
	check(load(parsed.items[4].payload) == 0xff85u, "open signed enum");
	for (std::size_t i : {5u, 6u})
		check(parsed.items[i].status == rm::ItemStatus::UnsupportedType &&
		          parsed.items[i].payload.empty(),
		      "small Struct/Array are not scalars");
	check(parsed.items[7].status == rm::ItemStatus::Unavailable && parsed.items[7].payload.empty(),
	      "unavailable item");
	check(parsed.items[8].status == rm::ItemStatus::NotFound && parsed.items[8].payload.empty(),
	      "unknown ID item");

	// Adapt only the coherent Bool read callback to return noncanonical bytes.
	auto boolRow = mixedFields[0];
	boolRow.read =
	    +[](const void*, std::byte* value, ts::Workspace&) noexcept -> ts::EncodedReadResult {
		++mixed.calls;
		value[0] = std::byte{2};
		return {ts::DispatchStatus::Ok, 1};
	};
	const ts::FieldCatalog boolCatalog{"bool", &boolRow, 1};
	auto boolModel = mixedModel.view();
	boolModel.fields = ts::FieldIndex{&boolCatalog, 1};
	const auto badBool =
	    ts::readManyEncoded(boolModel, std::span{mixedIds}.first(1), output, workspace);
	const auto boolReply =
	    rm::parseReply(Input{output}.first(badBool.written), std::span{mixedIds}.first(1));
	check(badBool.dispatch == ts::DispatchStatus::Ok && bool(boolReply) &&
	          boolReply.items[0].status == rm::ItemStatus::ReadError &&
	          boolReply.items[0].payload.empty(),
	      "noncanonical bool suppressed");
}

static void dispatcherChecks()
{
	alignas(16) std::array<std::byte, 128> scratch{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, 200> request{};
	std::array<std::byte, 600> reply{};
	const rm::Capability capability{7, 0x123456789abcdef0ULL};
	const std::array<ts::PackedId, 2> ids{0, 0x10000u};
	auto body = rm::writeRequest(Output{request}.subspan(32), ids);
	check(bool(body), "fixture request body");
	ew::Header header{};
	header.operation = ew::Operation::ReadMany;
	header.session = 7;
	header.requestId = 13;
	header.expectedResponseBytes = 514;
	header.payloadBytes = std::uint32_t(body.written);
	check(ew::writeHeader(request, header) == ew::Status::Ok, "fixture envelope");
	const auto input = Input{request}.first(32 + body.written);
	scalar.calls = 0;
	reply.fill(std::byte{0x5a});
	const auto before = reply;
	const auto disabled = server::dispatchEncoded(model.view(), workspace, 7, input, reply);
	check(disabled.status == server::EncodedDispatchStatus::NotAdmitted && scalar.calls == 0 &&
	          reply == before,
	      "compatibility overload never enables batch");
	const auto done = server::dispatchEncoded(model.view(), workspace, 7, input, reply, capability);
	check(bool(done) && scalar.calls == 2, "explicit admitted batch");
	const auto parsed = ew::parse(Input{reply}.first(done.written));
	check(bool(parsed) && parsed.header.expectedResponseBytes == 514 &&
	          parsed.header.payloadBytes == 34 && parsed.header.endpointStatus == 0,
	      "ceiling echoed while actual payload is shorter");
	check(bool(rm::parseReply(parsed.payload, ids)), "full reply body validation");
	const auto shortBuffer = server::dispatchEncoded(model.view(), workspace, 7, input,
	                                                 Output{reply}.first(66), capability);
	check(bool(shortBuffer) && shortBuffer.written == 66,
	      "transport need not reserve unused ceiling bytes");

	const auto global = [&](Input frame, rm::Capability cap, std::uint8_t dispatch,
	                        Output output = {}) {
		if (output.data() == nullptr)
			output = reply;
		scalar.calls = 0;
		reply.fill(std::byte{0x5a});
		const auto result = server::dispatchEncoded(model.view(), workspace, 7, frame, output, cap);
		check(bool(result) && result.written == 32 && scalar.calls == 0,
		      "global error precedes every getter");
		const auto error = ew::parse(Input{reply}.first(result.written));
		check(bool(error) && error.header.dispatch == dispatch && error.payload.empty(),
		      "correlated header-only error");
		check(reply[32] == std::byte{0x5a}, "global error does not touch payload storage");
	};
	global(input, capability, 6, Output{reply}.first(65));
	// Actual output capacity cannot override the advertised response ceiling.
	header.expectedResponseBytes = 33;
	check(ew::writeHeader(request, header) == ew::Status::Ok, "short response ceiling");
	global(input, capability, 6);
	header.expectedResponseBytes = 514;
	check(ew::writeHeader(request, header) == ew::Status::Ok, "restore response ceiling");
	for (unsigned kind = 0; kind < 4; ++kind) {
		auto cap = capability;
		if (kind == 0)
			cap.itemLimit = 1;
		if (kind == 1)
			cap.requestLimit = 6;
		if (kind == 2)
			cap.replyLimit = 513;
		if (kind == 3) {
			request[34] = request[38];
			request[35] = request[39];
			request[36] = request[40];
			request[37] = request[41];
		}
		global(input, cap, 5);
		body = rm::writeRequest(Output{request}.subspan(32), ids);
	}
	for (std::uint16_t count : {0u, 33u}) {
		request[32] = std::byte(count);
		request[33] = std::byte{0};
		global(input, capability, 5);
	}
	body = rm::writeRequest(Output{request}.subspan(32), ids);
	for (unsigned kind = 0; kind < 4; ++kind) {
		auto cap = capability;
		if (kind == 0)
			cap.session = 8;
		if (kind == 1)
			cap.itemLimit = 0;
		if (kind == 2)
			cap.replyLimit = 515;
		scalar.calls = 0;
		reply.fill(std::byte{0x5a});
		const auto snapshot = reply;
		const auto result =
		    server::dispatchEncoded(model.view(), workspace, kind == 3 ? 8 : 7, input, reply, cap);
		check(result.status == server::EncodedDispatchStatus::NotAdmitted && scalar.calls == 0 &&
		          reply == snapshot,
		      "session/profile admission before output mutation");
	}
	scalar.calls = 0;
	const auto small = server::dispatchEncoded(model.view(), workspace, 7, input,
	                                           Output{reply}.first(31), capability);
	check(small.status == server::EncodedDispatchStatus::BufferTooSmall && scalar.calls == 0,
	      "minimum header capacity");
	const auto overlap =
	    server::dispatchEncoded(model.view(), workspace, 7, input, request, capability);
	check(overlap.status == server::EncodedDispatchStatus::Overlap && scalar.calls == 0,
	      "request/reply overlap preflight");
	const auto smallOverlap = server::dispatchEncoded(model.view(), workspace, 7, input,
	                                                  Output{request}.first(31), capability);
	check(smallOverlap.status == server::EncodedDispatchStatus::BufferTooSmall && scalar.calls == 0,
	      "minimum header capacity precedes overlap");
	ts::Workspace requestWorkspace{request};
	const auto inputScratch =
	    server::dispatchEncoded(model.view(), requestWorkspace, 7, input, reply, capability);
	check(inputScratch.status == server::EncodedDispatchStatus::Overlap && scalar.calls == 0,
	      "request/workspace overlap preflight");
	const auto outputScratch =
	    server::dispatchEncoded(model.view(), workspace, 7, input, scratch, capability);
	check(outputScratch.status == server::EncodedDispatchStatus::Overlap && scalar.calls == 0,
	      "response/workspace overlap preflight");
	// Capability control bytes must not be overwritten by the reply or scratch.
	alignas(rm::Capability) std::array<std::byte, 600> controlStorage{};
	auto* overlappingCapability = ::new (controlStorage.data()) rm::Capability(capability);
	const auto controlSnapshot = controlStorage;
	const auto controlOutput = server::dispatchEncoded(model.view(), workspace, 7, input,
	                                                   controlStorage, *overlappingCapability);
	check(controlOutput.status == server::EncodedDispatchStatus::Overlap && scalar.calls == 0 &&
	          controlStorage == controlSnapshot,
	      "capability/reply overlap preserves control object");
	ts::Workspace controlWorkspace{controlStorage};
	const auto controlScratch = server::dispatchEncoded(model.view(), controlWorkspace, 7, input,
	                                                    reply, *overlappingCapability);
	check(controlScratch.status == server::EncodedDispatchStatus::Overlap && scalar.calls == 0 &&
	          controlStorage == controlSnapshot,
	      "capability/workspace overlap preserves control object");
	std::destroy_at(overlappingCapability);

	// The new overload does not change ordinary FieldRead admission or exact size.
	header.operation = ew::Operation::FieldRead;
	header.endpoint = 0;
	header.payloadBytes = 0;
	header.expectedResponseBytes = 8;
	check(ew::writeHeader(request, header) == ew::Status::Ok, "legacy request header");
	auto invalidCapability = capability;
	invalidCapability.session = 0;
	const auto legacy = server::dispatchEncoded(model.view(), workspace, 7,
	                                            Input{request}.first(32), reply, invalidCapability);
	check(bool(legacy) && legacy.written == 40, "legacy ignores unrelated optional capability");
}

int main()
{
	coreChecks();
	dispatcherChecks();
	std::printf("ReadMany server checks=%u failures=0\n", checks);
}

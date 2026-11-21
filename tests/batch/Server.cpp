/* Mixed Batch native Model/global preflight/complete-frame checks. MIT. */
#include <telemetry/Telemetry.hpp>
#include <telemetry/encoded/Batch.hpp>
#include <integration/encoded/BatchDispatcher.hpp>
#include <array>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace ts = telemetry;
namespace ew = ts::protocol::encoded;
namespace bw = ew::batch;
namespace server = ts::integration::encoded;
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
static unsigned checks = 0;

static void check(bool ok, unsigned line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "batch server check=%u line=%u\n", checks, line);
		std::abort();
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

struct Config {
	std::uint32_t value;
	bool enabled;
	std::array<bool, 2> flags;
};

struct Empty {};
enum class Mode : std::int16_t {
	Low = -3,
	High = 11
};

struct Owner {
	Config current{7, true, {false, true}};
	std::array<unsigned, 128> order{};
	unsigned calls = 0;

	void note(unsigned kind) noexcept
	{
		order[calls++] = kind;
	}

	Config read() noexcept
	{
		note(1);
		return current;
	}

	ts::WriteResult write(const Config& value) noexcept
	{
		note(2);
		current = value;
		return ts::WriteResult::Applied;
	}

	ts::WriteResult refuse(const Config&) noexcept
	{
		note(3);
		return ts::WriteResult::InvalidValue;
	}

	ts::CommandResult accepted(const Config&) noexcept
	{
		note(4);
		return ts::CommandResult::Accepted;
	}

	ts::CommandResult reset() noexcept
	{
		note(5);
		return ts::CommandResult::Executed;
	}

	ts::ServiceResult<Config> echo(const Config& value) noexcept
	{
		note(6);
		return ts::ServiceResult<Config>::success(value);
	}

	ts::ServiceResult<Config> busy(const Config&) noexcept
	{
		note(7);
		return ts::ServiceResult<Config>::failure(ts::ServiceStatus::Busy);
	}

	ts::ServiceResult<void> ping() noexcept
	{
		note(8);
		return ts::ServiceResult<void>::success();
	}

	ts::ServiceResult<Empty> empty() noexcept
	{
		note(9);
		return ts::ServiceResult<Empty>::success({});
	}

	float real() noexcept
	{
		note(10);
		return std::bit_cast<float>(0xff812345u);
	}

	Mode mode() noexcept
	{
		note(11);
		return static_cast<Mode>(-123);
	}
};

static Owner owner;
static ts::OwnerSlot<Owner> absent;
static constexpr ts::FieldTable fields{ts::field<&Owner::read, &Owner::write>("Config", owner),
                                       ts::field<&Owner::read>("ReadOnly", owner),
                                       ts::field<&Owner::read, &Owner::refuse>("Refuse", owner),
                                       ts::field<&Owner::read>("Missing", absent),
                                       ts::field<&Owner::real>("Real", owner),
                                       ts::field<&Owner::mode>("Mode", owner)};
static constexpr ts::ServiceTable services{
    ts::service<&Owner::echo>("Echo", owner), ts::service<&Owner::busy>("Busy", owner),
    ts::service<&Owner::ping>("Ping", owner), ts::service<&Owner::empty>("Empty", owner)};
// One group, device, of every family: each family's IDs are 0, 1, ...
static constexpr ts::Model model{ts::group(
    "device", ts::field<&Owner::read, &Owner::write>("Config", owner),
    ts::field<&Owner::read>("ReadOnly", owner),
    ts::field<&Owner::read, &Owner::refuse>("Refuse", owner),
    ts::field<&Owner::read>("Missing", absent), ts::field<&Owner::real>("Real", owner),
    ts::field<&Owner::mode>("Mode", owner), ts::command<&Owner::accepted>("Accepted", owner),
    ts::command<&Owner::reset>("Reset", owner), ts::service<&Owner::echo>("Echo", owner),
    ts::service<&Owner::busy>("Busy", owner), ts::service<&Owner::ping>("Ping", owner),
    ts::service<&Owner::empty>("Empty", owner))};

static auto config(std::uint32_t value)
{
	std::array<std::byte, 7> result{};
	for (unsigned i = 0; i < 4; ++i)
		result[i] = std::byte((value >> (8 * i)) & 255);
	result[4] = result[6] = std::byte{1};
	return result;
}

static std::uint64_t load(Input input)
{
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < input.size(); ++i)
		value |= std::uint64_t{std::to_integer<unsigned>(input[i])} << (8 * i);
	return value;
}

static bw::ReplyResult parse(Input output, std::span<const ts::BatchEncodedItem> items)
{
	std::array<bw::RequestItem, 32> expected{};
	for (std::size_t i = 0; i < items.size(); ++i)
		expected[i] = {static_cast<ew::Operation>(items[i].operation), items[i].id,
		               items[i].payload};
	return bw::parseReply(output, std::span{expected}.first(items.size()));
}

static void coreChecks()
{
	alignas(16) std::array<std::byte, 4096> scratch{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, 2048> output{};
	const auto a = config(99), b = config(123);
	const std::array mixed{ts::BatchEncodedItem{ts::Operation::FieldWrite, 0, a},
	                       ts::BatchEncodedItem{ts::Operation::Command, 0, a},
	                       ts::BatchEncodedItem{ts::Operation::Service, 1, a},
	                       ts::BatchEncodedItem{ts::Operation::Service, 0, b},
	                       ts::BatchEncodedItem{ts::Operation::FieldRead, 0, {}}};
	owner.calls = 0;
	auto done = ts::batchEncoded(model.view(), mixed, output, workspace);
	CHECK(done.dispatch == ts::DispatchStatus::Ok && owner.calls == 5 && workspace.used() == 0);
	CHECK(owner.order[0] == 2 && owner.order[1] == 4 && owner.order[2] == 7 &&
	      owner.order[3] == 6 && owner.order[4] == 1);
	auto reply = parse(Input{output}.first(done.written), mixed);
	CHECK(reply && reply.count == 5 && done.written == 2 + 12 * 5 + 14);
	CHECK(reply.items[0].endpointStatus == unsigned(ts::WriteResult::Applied));
	CHECK(reply.items[1].endpointStatus == unsigned(ts::CommandResult::Accepted));
	CHECK(reply.items[2].endpointStatus == unsigned(ts::ServiceStatus::Busy) &&
	      reply.items[2].payload.empty());
	CHECK(std::equal(reply.items[3].payload.begin(), reply.items[3].payload.end(), b.begin()));
	CHECK(std::equal(reply.items[4].payload.begin(), reply.items[4].payload.end(), a.begin()));

	// Independent statuses, exact special float bits and signed open enum, plus
	// distinct zero-wire Void and empty struct services all survive one response.
	const std::array outcomes{ts::BatchEncodedItem{ts::Operation::FieldWrite, 2, a},
	                          ts::BatchEncodedItem{ts::Operation::FieldRead, 3, {}},
	                          ts::BatchEncodedItem{ts::Operation::FieldRead, 4, {}},
	                          ts::BatchEncodedItem{ts::Operation::FieldRead, 5, {}},
	                          ts::BatchEncodedItem{ts::Operation::Service, 2, {}},
	                          ts::BatchEncodedItem{ts::Operation::Service, 3, {}},
	                          ts::BatchEncodedItem{ts::Operation::Command, 1, {}}};
	owner.calls = 0;
	done = ts::batchEncoded(model.view(), outcomes, output, workspace);
	reply = parse(Input{output}.first(done.written), outcomes);
	CHECK(done.dispatch == ts::DispatchStatus::Ok && reply && owner.calls == 6);
	CHECK(reply.items[0].endpointStatus == unsigned(ts::WriteResult::InvalidValue));
	CHECK(reply.items[1].dispatch == unsigned(ts::DispatchStatus::Unavailable) &&
	      reply.items[1].payload.empty());
	CHECK(load(reply.items[2].payload) == 0xff812345u && load(reply.items[3].payload) == 0xff85u);
	CHECK(reply.items[4].dispatch == 0 && reply.items[4].payload.empty());
	CHECK(reply.items[5].dispatch == 0 && reply.items[5].payload.empty());
	CHECK(reply.items[6].endpointStatus == unsigned(ts::CommandResult::Executed));

	std::array<ts::BatchEncodedItem, 32> repeated{};
	for (std::size_t i = 0; i < repeated.size(); ++i)
		repeated[i] = i % 2 == 0 ? ts::BatchEncodedItem{ts::Operation::FieldWrite, 0,
		                                                i < 16 ? Input{a} : Input{b}}
		                         : ts::BatchEncodedItem{ts::Operation::FieldRead, 0, {}};
	owner.calls = 0;
	done = ts::batchEncoded(model.view(), repeated, output, workspace);
	reply = parse(Input{output}.first(done.written), repeated);
	CHECK(done.dispatch == ts::DispatchStatus::Ok && reply && reply.count == 32 &&
	      owner.calls == 32);
	for (std::size_t i = 1; i < repeated.size(); i += 2)
		CHECK(load(reply.items[i].payload.first(4)) == (i < 16 ? 99 : 123));

	const auto reject = [&](std::span<const ts::BatchEncodedItem> items, ts::DispatchStatus status,
	                        Output destination, ts::Workspace& ws, ts::ModelView view) {
		owner.calls = 0;
		output.fill(std::byte{0x5a});
		const auto before = output;
		const auto result = ts::batchEncoded(view, items, destination, ws);
		CHECK(result.dispatch == status && result.written == 0 && owner.calls == 0);
		CHECK(output == before && ws.used() == 0);
	};
	for (unsigned kind = 0; kind < 7; ++kind) {
		auto items = mixed;
		auto bad = a;
		if (kind == 0)
			items.back().id = 999;
		if (kind == 1)
			items.back() = {ts::Operation::FieldWrite, 1, a};
		if (kind == 2) {
			bad[4] = std::byte{2};
			items.back() = {ts::Operation::Service, 0, bad};
		}
		if (kind == 3) {
			bad[6] = std::byte{2};
			items.back() = {ts::Operation::Command, 0, bad};
		}
		if (kind == 4)
			items.back() = {ts::Operation::Service, 0, Input{a}.first(6)};
		if (kind == 5)
			items.back().operation = static_cast<ts::Operation>(5);
		if (kind == 6)
			items.back() = {ts::Operation::FieldRead, 0, a};
		reject(items, kind == 0 ? ts::DispatchStatus::NotFound : ts::DispatchStatus::InvalidPayload,
		       output, workspace, model.view());
	}
	reject({}, ts::DispatchStatus::InvalidPayload, output, workspace, model.view());
	std::array<ts::BatchEncodedItem, 33> tooMany{};
	reject(tooMany, ts::DispatchStatus::InvalidPayload, output, workspace, model.view());
	reject(mixed, ts::DispatchStatus::BufferTooSmall, Output{output}.first(75), workspace,
	       model.view());
	std::array<std::byte, 65535> oversized{};
	auto huge = mixed;
	huge.back() = {ts::Operation::Service, 0, oversized};
	reject(huge, ts::DispatchStatus::InvalidPayload, output, workspace, model.view());
	auto overlapping = mixed;
	overlapping.back() = {ts::Operation::Service, 0, Input{output}.first(7)};
	reject(overlapping, ts::DispatchStatus::InvalidPayload, output, workspace, model.view());
	// Scratch is checked globally even when only the last operation requires it.
	std::array<ts::FieldEntry, fields.staticSize> rows{};
	std::copy_n(fields.data(), rows.size(), rows.begin());
	rows[0].readScratchBytes = 4097;
	const ts::FieldCatalog group{"device", rows.data(), static_cast<std::uint32_t>(rows.size())};
	auto adapted = model.view();
	adapted.fields = ts::FieldIndex{&group, 1};
	reject(mixed, ts::DispatchStatus::WorkspaceTooSmall, output, workspace, adapted);
	rows[0].readScratchBytes = 0;
	rows[0].wireBytes = 6;
	reject(mixed, ts::DispatchStatus::InvalidPayload, output, workspace, adapted);
	// A cyclic forged type reference cannot turn preflight into unbounded work.
	std::array<ts::TypeDescriptor, 64> types{};
	CHECK(model.view().types.count <= types.size());
	std::copy_n(model.view().types.data, model.view().types.count, types.begin());
	auto cyclic = model.view();
	cyclic.types.data = types.data();
	const auto configType = *cyclic.fieldTypeId(0);
	types[configType].kind = ts::TypeKind::Array;
	types[configType].relatedTypeId = configType;
	types[configType].elementCount = 1;
	reject(mixed, ts::DispatchStatus::InvalidPayload, output, workspace, cyclic);
	const auto overlapsScratch = mixed;
	std::array<ts::BatchEncodedItem, mixed.size()> scratchItems = overlapsScratch;
	scratchItems.back() = {ts::Operation::Service, 0, Input{scratch}.first(7)};
	reject(scratchItems, ts::DispatchStatus::InvalidPayload, output, workspace, model.view());
	rows[0] = fields[0];
	rows[0].read =
	    +[](const void*, std::byte* bytes, ts::Workspace&) noexcept -> ts::EncodedReadResult {
		owner.note(12);
		std::fill_n(bytes, 7, std::byte{0xee});
		return {ts::DispatchStatus::InvalidPayload, 7};
	};
	const std::array partial{ts::BatchEncodedItem{ts::Operation::FieldRead, 0, {}},
	                         ts::BatchEncodedItem{ts::Operation::FieldRead, 4, {}}};
	owner.calls = 0;
	done = ts::batchEncoded(adapted, partial, output, workspace);
	reply = parse(Input{output}.first(done.written), partial);
	CHECK(done.dispatch == ts::DispatchStatus::Ok && reply && owner.calls == 2 &&
	      done.written == 30);
	CHECK(reply.items[0].dispatch == unsigned(ts::DispatchStatus::InvalidPayload) &&
	      reply.items[0].payload.empty());
	CHECK(reply.items[1].dispatch == 0 && load(reply.items[1].payload) == 0xff812345u);
	rows[0].read =
	    +[](const void*, std::byte* bytes, ts::Workspace&) noexcept -> ts::EncodedReadResult {
		owner.note(12);
		std::fill_n(bytes, 7, std::byte{2});
		return {ts::DispatchStatus::Ok, 7};
	};
	done = ts::batchEncoded(adapted, partial, output, workspace);
	reply = parse(Input{output}.first(done.written), partial);
	CHECK(reply && reply.items[0].dispatch == unsigned(ts::DispatchStatus::InternalError));
	CHECK(reply.items[0].payload.empty() && reply.items[1].dispatch == 0);
}

static void dispatchChecks()
{
	alignas(16) std::array<std::byte, 256> scratch{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, 2048> input{}, output{};
	const auto value = config(987);
	const std::array items{bw::RequestItem{ew::Operation::FieldWrite, 0, value},
	                       bw::RequestItem{ew::Operation::FieldRead, 0, {}}};
	const auto body = bw::writeRequest(Output{input}.subspan(32), items);
	CHECK(body);
	ew::Header header{};
	header.operation = ew::Operation::Batch;
	header.session = 7;
	header.requestId = 99;
	header.payloadBytes = static_cast<std::uint32_t>(body.written);
	header.expectedResponseBytes = 65535;
	CHECK(ew::writeHeader(input, header) == ew::Status::Ok);
	const auto request = Input{input}.first(32 + body.written);
	const bw::Capability capability{7, 0x123456789abcdef0ULL};
	owner.calls = 0;
	output.fill(std::byte{0x5a});
	const auto before = output;
	auto result = server::dispatchEncoded(model.view(), workspace, 7, request, output);
	CHECK(result.status == server::EncodedDispatchStatus::NotAdmitted && owner.calls == 0 &&
	      output == before);
	result = server::dispatchEncoded(model.view(), workspace, 7, request, output, capability);
	CHECK(result && owner.calls == 2);
	auto parsed = ew::parse(Input{output}.first(result.written));
	CHECK(parsed && parsed.header.expectedResponseBytes == 65535 && parsed.header.dispatch == 0);
	auto reply = bw::parseReply(parsed.payload, items);
	CHECK(reply && reply.count == 2 && load(reply.items[1].payload.first(4)) == 987);
	for (unsigned kind = 0; kind < 4; ++kind) {
		auto cap = capability;
		if (kind == 0)
			cap.session = 8;
		if (kind == 1)
			cap.itemLimit = 0;
		if (kind == 2)
			cap.requestLimit = 9;
		if (kind == 3)
			cap.replyLimit = 13;
		owner.calls = 0;
		output.fill(std::byte{0x5a});
		result = server::dispatchEncoded(model.view(), workspace, 7, request, output, cap);
		CHECK(result.status == server::EncodedDispatchStatus::NotAdmitted && owner.calls == 0 &&
		      output == before);
	}
	owner.calls = 0;
	result = server::dispatchEncoded(model.view(), workspace, 7, request, Output{output}.first(62),
	                                 capability);
	parsed = ew::parse(Input{output}.first(result.written));
	CHECK(result && result.written == 32 && parsed &&
	      parsed.header.dispatch == unsigned(ts::DispatchStatus::BufferTooSmall));
	CHECK(owner.calls == 0 && output[32] == std::byte{0x5a});
	result = server::dispatchEncoded(model.view(), workspace, 7, request, input, capability);
	CHECK(result.status == server::EncodedDispatchStatus::Overlap && owner.calls == 0);
	result = server::dispatchEncoded(model.view(), workspace, 7, request, Output{output}.first(31),
	                                 capability);
	CHECK(result.status == server::EncodedDispatchStatus::BufferTooSmall && owner.calls == 0);
	// The maximum advertised body is a budget, never permission to skip checking
	// the real response storage or a reduced discovered capability.
	auto limited = capability;
	limited.itemLimit = 1;
	result = server::dispatchEncoded(model.view(), workspace, 7, request, output, limited);
	parsed = ew::parse(Input{output}.first(result.written));
	CHECK(result && parsed &&
	      parsed.header.dispatch == unsigned(ts::DispatchStatus::InvalidPayload) &&
	      owner.calls == 0);
	header.operation = ew::Operation::FieldRead;
	header.endpoint = 0;
	header.payloadBytes = 0;
	header.expectedResponseBytes = 7;
	CHECK(ew::writeHeader(input, header) == ew::Status::Ok);
	limited.session = 0;
	result = server::dispatchEncoded(model.view(), workspace, 7, Input{input}.first(32), output,
	                                 limited);
	parsed = ew::parse(Input{output}.first(result.written));
	CHECK(result && parsed && parsed.header.dispatch == 0 && parsed.payload.size() == 7);
}

int main()
{
	coreChecks();
	dispatchChecks();
	std::printf("Batch server checks=%u failures=0\n", checks);
}

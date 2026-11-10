/* Optional Workbench reads preserve snapshots, drafts and the single FIFO. MIT. */
#include <integration/batch/Workbench.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/BatchDispatcher.hpp>
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ts = telemetry;
namespace cl = ts::client;
namespace rt = ts::runtime;
namespace dy = ts::integration::dynamic;
namespace im = ts::integration::inspector;
namespace wb = ts::integration::workbench;
namespace bt = ts::integration::batch;
namespace en = ts::integration::encoded;
static unsigned checks = 0;
#define CHECK(...)                                                                                 \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(__VA_ARGS__)) {                                                                      \
			std::fprintf(stderr, "Batch Workbench check=%u line=%u failed\n", checks, __LINE__);   \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

struct Request {
	std::uint32_t value;
};

struct Response {
	std::uint64_t value;
};

struct Owner {
	std::uint32_t value = 17;
	std::uint64_t floatBits = UINT64_C(0x7ff8000000001234);

	double readFloat() const noexcept
	{
		return std::bit_cast<double>(floatBits);
	}

	ts::WriteResult writeFloat(double next) noexcept
	{
		floatBits = std::bit_cast<std::uint64_t>(next);
		return ts::WriteResult::Applied;
	}

	ts::CommandResult noArgs() noexcept
	{
		return ts::CommandResult::Executed;
	}

	unsigned writes = 0, commands = 0, calls = 0;

	std::uint32_t read() const noexcept
	{
		return value;
	}

	std::uint64_t wide() const noexcept
	{
		return UINT64_MAX;
	}

	bool flag() const noexcept
	{
		return true;
	}

	ts::WriteResult write(std::uint32_t next) noexcept
	{
		++writes;
		value = next;
		return ts::WriteResult::Applied;
	}

	ts::CommandResult command(Request) noexcept
	{
		++commands;
		return ts::CommandResult::Executed;
	}

	ts::ServiceResult<Response> service(Request) noexcept
	{
		++calls;
		return ts::ServiceResult<Response>::success(Response{UINT64_MAX});
	}
};

static Owner owner;
static constexpr ts::FieldTable fieldTable{
    ts::field<&Owner::read, &Owner::write>("Value", owner), ts::field<&Owner::wide>("Wide", owner),
    ts::field<&Owner::flag>("Flag", owner),
    ts::field<&Owner::readFloat, &Owner::writeFloat>("Float", owner)};
static constexpr ts::FieldCatalogTable fields{ts::group("values", fieldTable)};
static constexpr ts::CommandTable commandTable{ts::command<&Owner::command>("Command", owner),
                                               ts::command<&Owner::noArgs>("Void", owner)};
static constexpr ts::CommandCatalogTable commands{ts::group("requests", commandTable)};
static constexpr ts::ServiceTable serviceTable{ts::service<&Owner::service>("Service", owner)};
static constexpr ts::ServiceCatalogTable services{ts::group("requests", serviceTable)};
static constexpr ts::Model native{fields, commands, services};
static constexpr ts::descriptor::Descriptor descriptor{native};
static constexpr auto bytes = ts::descriptor::packDescriptor<descriptor>();

struct Metadata {
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 8> members{};
	std::array<rt::RuntimeEnumEntry, 1> enums{};
	std::array<rt::RuntimeCatalog, 4> catalogs{};
	std::array<rt::RuntimeEndpoint, 8> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	rt::RuntimeSchema schema;

	Metadata() noexcept
	{
		const auto parsed = rt::RuntimeSchema::parse(bytes, workspace);
		CHECK(parsed);
		schema = parsed.schema;
	}
};

template<std::size_t N = 4>
struct Harness {
	Metadata metadata;
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* context) noexcept {
		                return static_cast<Harness*>(context)->now;
	                }};
	std::array<std::byte, 1024> tx{}, rx{}, port{}, reply{}, scratch{};
	ts::Workspace workspace{scratch};
	std::size_t replyBytes = 0;
	en::EncodedBackend backend{tx, rx, clock, 30000, true, true};
	std::array<std::byte, 64> serviceStorage{};
	wb::WorkbenchModel<N, 512, 64, 1536> workbench{backend, clock, serviceStorage};
	bt::wire::Capability capability{17, metadata.schema.fingerprint()};

	Harness() noexcept
	{
		owner = {};
		CHECK(workbench.admit(metadata.schema, cl::Input{bytes}.first(64), 17) == cl::Status::Ok);
	}

	~Harness()
	{
		workbench.close();
		workbench.process(now);
		CHECK(workbench.drained());
	}

	auto field(std::uint32_t id = 0) noexcept
	{
		const auto binding = workbench.bindField(id);
		CHECK(binding);
		return binding.endpoint;
	}

	auto batchBinding(std::span<const bt::Endpoint> endpoints) noexcept
	{
		const auto binding = bt::bindBatch(workbench, backend, capability, endpoints);
		CHECK(binding);
		return binding.binding;
	}

	cl::Packet offered() noexcept
	{
		auto packet = workbench.nextPacket();
		CHECK(packet);
		std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
		packet.bytes = cl::Input{port}.first(packet.bytes.size());
		return packet;
	}

	void send(cl::Packet packet, bool deliver = true) noexcept
	{
		CHECK(workbench.prepareHandoff(packet.session, packet.requestId));
		CHECK(workbench.onSendAccepted(packet.session, packet.requestId));
		const auto result = en::dispatchEncoded(native.view(), workspace, packet.session,
		                                        packet.bytes, reply, capability);
		CHECK(result);
		replyBytes = result.written;
		if (deliver)
			CHECK(workbench.onReply(cl::Input{reply}.first(result.written), packet.session,
			                        packet.requestId));
	}

	void pump() noexcept
	{
		for (unsigned i = 0; i < 32 && workbench.pending(); ++i) {
			workbench.process(now);
			if (const auto packet = workbench.nextPacket()) {
				std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
				auto owned = packet;
				owned.bytes = cl::Input{port}.first(packet.bytes.size());
				send(owned);
			}
			workbench.process(now);
		}
		CHECK(workbench.pending() == 0);
	}

	im::NodeHandle snapshot() noexcept
	{
		unsigned callbacks = 0;
		CHECK(workbench.readField(field(), [&](const im::ReadResult& result) noexcept {
			++callbacks;
			CHECK(result.published);
		}));
		pump();
		CHECK(callbacks == 1 && workbench.root());
		return workbench.root().value;
	}
};

template<class W>
[[maybe_unused]] static std::uint32_t value(W& workbench, im::NodeHandle root,
                                            bool edited = false) noexcept
{
	const auto result = edited ? workbench.editScalar(root) : workbench.scalar(root);
	CHECK(result);
	const auto number = std::get_if<std::uint32_t>(&result.value.value);
	CHECK(number);
	return *number;
}

static constexpr std::array<bt::Endpoint, 3> sequence{
    {{cl::Operation::FieldWrite, 0}, {cl::Operation::Command, 0}, {cl::Operation::FieldRead, 0}}};
static constexpr std::array<std::byte, 4> updated{std::byte{99}, std::byte{}, std::byte{},
                                                  std::byte{}};
static constexpr std::array<cl::Input, 3> payloads{cl::Input{updated}, cl::Input{updated},
                                                   cl::Input{}};

void acceptance()
{
	Harness h;
	auto& w = h.workbench;
	const auto root = h.snapshot();
	CHECK(w.beginEdit() == rt::Status::Ok &&
	      w.setScalar(root, std::uint32_t{88}) == rt::Status::Ok);
	std::array<std::byte, 512> request{};
	const auto binding = h.batchBinding(sequence);
	unsigned completed = 0;
	auto malformed = payloads;
	malformed[1] = {};
	CHECK(bt::batch(w, binding, malformed, request, [&](const auto&) noexcept {
		      ++completed;
	      }).status == cl::Status::InvalidArgument);
	CHECK(w.editing() && w.dirty() && value(w, root, true) == 88 && w.info(root));
	CHECK(bt::batch(w, binding, payloads, request, [&](const bt::BatchResult& result) noexcept {
		++completed;
		CHECK(result.operation.succeeded() && result.count == 3);
		CHECK(result.items[0].operation.succeeded() && result.items[1].operation.succeeded());
		CHECK(result.items[2].hasValue() && result.items[2].value.get<std::uint32_t>().value == 99);
		CHECK(bt::bindBatch(w, h.backend, h.capability, sequence).status == cl::Status::Busy);
	}));
	CHECK(!w.editing() && !w.info(root) && owner.writes == 0 && owner.commands == 0);
	CHECK(w.phase() == wb::Phase::Executing && completed == 0);
	h.pump();
	CHECK(completed == 1 && owner.writes == 1 && owner.commands == 1 &&
	      w.phase() == wb::Phase::Idle);
}

void refusalAndReadOnly()
{
	Harness<1> h;
	auto& w = h.workbench;
	const auto root = h.snapshot();
	CHECK(w.beginEdit() == rt::Status::Ok &&
	      w.setScalar(root, std::uint32_t{88}) == rt::Status::Ok);
	const auto binding = h.batchBinding(sequence);
	std::array<std::byte, 512> request{};
	CHECK(w.readField(h.field(1), [](const auto&) noexcept {
	}));
	// Read selection legitimately retires the old editor source; start fresh
	// after this ordinary request, then exercise the optional one-slot FIFO.
	h.pump();
	CHECK(!w.canEdit());
	const auto scalar = h.snapshot();
	CHECK(w.beginEdit() == rt::Status::Ok &&
	      w.setScalar(scalar, std::uint32_t{88}) == rt::Status::Ok);
	const std::array<bt::Endpoint, 1> readEndpoints{{{cl::Operation::FieldRead, 1}}};
	const auto readBinding = h.batchBinding(readEndpoints);
	const std::array<cl::Input, 1> empty{};
	CHECK(bt::batch(w, readBinding, empty, request, [](const auto& result) noexcept {
		CHECK(result.items[0].hasValue());
	}));
	CHECK(bt::batch(w, binding, payloads, request, [](const auto&) noexcept {
	      }).status == cl::Status::QueueFull);
	CHECK(w.editing() && w.dirty() && w.info(scalar) && value(w, scalar, true) == 88);
	h.pump();
	CHECK(w.editing() && w.info(scalar) && owner.writes == 0 && owner.commands == 0);
}

void boundaries()
{
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness h;
		auto& w = h.workbench;
		std::array<std::byte, 512> request{};
		const auto binding = h.batchBinding(sequence);
		unsigned callbacks = 0;
		cl::Status status = cl::Status::Ok;
		CHECK(bt::batch(
		    w, binding, payloads, request,
		    [&](const auto& result) noexcept {
			    ++callbacks;
			    status = result.operation.status;
		    },
		    20));
		if (phase) {
			const auto packet = h.offered();
			if (phase == 2)
				CHECK(w.prepareHandoff(packet.session, packet.requestId));
		}
		h.now = 20;
		w.process(h.now);
		CHECK(callbacks == 1 &&
		      status == (phase == 2 ? cl::Status::OutcomeUnknown : cl::Status::Timeout));
		CHECK(w.recoveryRequired() == (phase == 2));
		w.process(h.now);
		CHECK(callbacks == 1 && owner.writes == 0 && owner.commands == 0);
		w.invalidate();
		CHECK(bt::bindBatch(w, h.backend, h.capability, sequence).status ==
		      (phase == 2 ? cl::Status::RecoveryRequired : cl::Status::NotAdmitted));
		w.close();
		CHECK(bt::bindBatch(w, h.backend, h.capability, sequence).status == cl::Status::Closed);
	}
}

void capture()
{
	Harness h;
	auto& w = h.workbench;
	const auto root = h.snapshot();
	CHECK(w.beginEdit() == rt::Status::Ok &&
	      w.setScalar(root, std::uint32_t{88}) == rt::Status::Ok);
	std::array<std::byte, 8> output{};
	output.fill(std::byte{0x5a});
	const auto before = output;
	CHECK(w.copyDraft(cl::Output{output}.first(3)).status == rt::Status::Capacity &&
	      output == before);
	const auto copied = w.copyDraft(output);
	CHECK(copied && copied.value == 4 && output[0] == std::byte{88} &&
	      output[4] == std::byte{0x5a});
	CHECK(w.editing() && w.dirty() && value(w, root, true) == 88 && value(w, root) == 17);
	CHECK(w.copyDraft(output) && w.editing() && w.dirty());
	CHECK(w.copyDraft({reinterpret_cast<std::byte*>(&w), sizeof(w)}).status == rt::Status::Overlap);
	w.invalidate();
	const auto stable = output;
	CHECK(w.copyDraft(output).status == rt::Status::Stale && output == stable);
	// RequestEditor itself also refuses a consumed writer without rearming it.
	Harness other;
	other.snapshot();
	const auto info = other.workbench.snapshotInfo();
	im::SnapshotStore source{other.serviceStorage};
	CHECK(source.admit(other.metadata.schema, 17) == im::ModelStatus::Ok);
	CHECK(source.select(0, info.type) == im::ModelStatus::Ok);
	const std::array<std::byte, 4> original{std::byte{17}, std::byte{}, std::byte{}, std::byte{}};
	CHECK(source.publish(original, source.admission(), source.revision()) == im::ModelStatus::Ok);
	std::array<std::byte, 8> draft{}, committed{};
	wb::RequestEditor editor{draft, committed};
	CHECK(editor.begin(source) == rt::Status::Ok);
	CHECK(editor.commit());
	CHECK(editor.copyDraft(output).status == rt::Status::Stale && output == stable);
}

void staleResult()
{
	Harness h;
	auto& w = h.workbench;
	std::array<std::byte, 512> request{};
	const auto binding = h.batchBinding(sequence);
	unsigned callbacks = 0;
	CHECK(bt::batch(w, binding, payloads, request, [&](const bt::BatchResult& result) noexcept {
		++callbacks;
		CHECK(result.operation.succeeded() && result.count == 3 &&
		      result.valueStatus == dy::ValueStatus::Stale);
		CHECK(result.items[0].operation.succeeded() && result.items[1].operation.succeeded());
		CHECK(!result.items[2].hasValue() && result.items[2].valueStatus == dy::ValueStatus::Stale);
	}));
	h.send(h.offered(), false);
	CHECK(owner.writes == 1 && owner.commands == 1 && callbacks == 0);
	w.invalidate();
	CHECK(w.onReply(cl::Input{h.reply}.first(h.replyBytes), 17, 1));
	CHECK(callbacks == 1 && w.phase() == wb::Phase::Idle);
}

void captureBitsAndVoid()
{
	Harness h;
	auto& w = h.workbench;
	for (const auto bits :
	     {UINT64_C(0x7ff8000000001234), UINT64_C(0xfff8000000004321), UINT64_C(0x7ff0000000000056),
	      UINT64_C(0xfff0000000000065), UINT64_C(0), UINT64_C(0x8000000000000000)}) {
		owner.floatBits = bits;
		CHECK(w.readField(h.field(3), [](const auto& result) noexcept {
			CHECK(result.published);
		}));
		h.pump();
		CHECK(w.beginEdit() == rt::Status::Ok);
		std::array<std::byte, 8> output{};
		const auto copied = w.copyDraft(output);
		CHECK(copied && copied.value == 8 && w.editing() && !w.dirty());
		std::uint64_t captured = 0;
		for (unsigned i = 0; i < 8; ++i)
			captured |= std::uint64_t(std::to_integer<unsigned>(output[i])) << (8 * i);
		CHECK(captured == bits);
		w.cancelEdit();
	}
	const auto command = w.bindCommand(1);
	CHECK(command && w.selectCommand(command.endpoint) == cl::Status::Ok);
	std::array<std::byte, 4> output{};
	output.fill(std::byte{0x7a});
	const auto before = output;
	const auto captured = w.copyDraft(output);
	CHECK(captured && captured.value == 0 && output == before && w.commandSelected() &&
	      w.editing());
	// Corrupt externally-owned staging: capture fails without publishing partial bytes.
	im::SnapshotStore source{h.serviceStorage};
	CHECK(source.admit(h.metadata.schema, 17) == im::ModelStatus::Ok);
	const auto flag = w.bindField(2);
	CHECK(flag && source.select(2, flag.endpoint.responseType()) == im::ModelStatus::Ok);
	const std::array<std::byte, 1> yes{std::byte{1}};
	CHECK(source.publish(yes, source.admission(), source.revision()) == im::ModelStatus::Ok);
	std::array<std::byte, 8> draft{}, committed{};
	wb::RequestEditor editor{draft, committed};
	CHECK(editor.begin(source) == rt::Status::Ok);
	draft[0] = std::byte{2};
	CHECK(editor.copyDraft(output).status == rt::Status::InvalidPayload && output == before &&
	      editor.current());
}

int main()
{
	acceptance();
	refusalAndReadOnly();
	boundaries();
	capture();
	captureBitsAndVoid();
	staleResult();
	std::printf("Batch Workbench: %u checks, 0 failures\n", checks);
	return 0;
}

/* Optional Workbench reads preserve snapshots, drafts and the single FIFO. MIT. */
#include <integration/read_many/Workbench.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/Dispatcher.hpp>
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
namespace rm = ts::integration::read_many;
namespace en = ts::integration::encoded;
static unsigned checks = 0;
#define CHECK(...)                                                                                 \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(__VA_ARGS__)) {                                                                      \
			std::fprintf(stderr, "ReadMany Workbench check=%u line=%u failed\n", checks,           \
			             __LINE__);                                                                \
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
// One group: every family's IDs start at 0, as the requests below send.
static constexpr ts::Model native{
    ts::group("values", ts::field<&Owner::read, &Owner::write>("Value", owner),
              ts::field<&Owner::wide>("Wide", owner), ts::field<&Owner::flag>("Flag", owner),
              ts::command<&Owner::command>("Command", owner),
              ts::service<&Owner::service>("Service", owner))};
static constexpr ts::descriptor::Descriptor descriptor{native};
static constexpr auto bytes = ts::descriptor::packDescriptor<descriptor>();
static constexpr std::array<std::uint32_t, 2> ids{1, 2};

struct Metadata {
	std::array<rt::RuntimeType, 16> types{};
	std::array<rt::RuntimeMember, 8> members{};
	std::array<rt::RuntimeEnumEntry, 1> enums{};
	std::array<rt::RuntimeCatalog, 6> catalogs{};
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
	en::EncodedBackend backend{tx, rx, clock, 30000, true};
	std::array<std::byte, 64> serviceStorage{};
	wb::WorkbenchModel<N, 130, 64, 768> workbench{backend, clock, serviceStorage};
	rm::wire::Capability capability{17, metadata.schema.fingerprint()};

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

	auto many() noexcept
	{
		const auto binding = rm::bindReadMany(workbench, backend, capability, ids);
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
static std::uint32_t value(W& workbench, im::NodeHandle root, bool edited = false) noexcept
{
	const auto result = edited ? workbench.editScalar(root) : workbench.scalar(root);
	CHECK(result);
	const auto number = std::get_if<std::uint32_t>(&result.value.value);
	CHECK(number);
	return *number;
}

static void preservedDraftAndFifo() noexcept
{
	Harness<2> h;
	auto& w = h.workbench;
	const auto root = h.snapshot();
	CHECK(w.beginEdit() == rt::Status::Ok &&
	      w.setScalar(root, std::uint32_t{99}) == rt::Status::Ok);
	const auto before = w.snapshotInfo();
	const auto binding = h.many();
	void (*nullMany)(const rm::ReadManyResult&) noexcept = nullptr;
	void (*nullField)(const dy::DynamicResult&) noexcept = nullptr;
	CHECK(rm::readMany(w, binding, nullMany).status == cl::Status::InvalidArgument);
	CHECK(rm::readFieldValue(w, h.field(), nullField).status == cl::Status::InvalidArgument);
	CHECK(rm::readMany(
	          w, binding,
	          [](const auto&) noexcept {
	          },
	          0)
	          .status == cl::Status::InvalidArgument);
	unsigned many = 0, fallback = 0, order = 0;
	CHECK(rm::readMany(w, binding, [&](const rm::ReadManyResult& result) noexcept {
		CHECK(order++ == 0 && result.operation.succeeded() && result.count == 2);
		CHECK(result.items[0].value.get<std::uint64_t>().value == UINT64_MAX);
		CHECK(result.items[1].value.get<bool>().value);
		CHECK(rm::readMany(w, binding, [](const auto&) noexcept {
		      }).status == cl::Status::Busy);
		++many;
	}));
	CHECK(rm::readFieldValue(w, h.field(1), [&](const dy::DynamicResult& result) noexcept {
		CHECK(order++ == 1 && result.hasValue());
		CHECK(result.value.get<std::uint64_t>().value == UINT64_MAX);
		++fallback;
	}));
	CHECK(w.pending() == 2);
	CHECK(rm::readMany(w, binding, [](const auto&) noexcept {
	      }).status == cl::Status::QueueFull);
	CHECK(rm::readFieldValue(w, h.field(), [](const auto&) noexcept {
	      }).status == cl::Status::QueueFull);
	CHECK(w.snapshotInfo().revision == before.revision &&
	      w.snapshotInfo().fieldId == before.fieldId);
	CHECK(w.editing() && w.dirty() && value(w, root, true) == 99 && value(w, root) == 17);
	h.pump();
	CHECK(many == 1 && fallback == 1 && order == 2 && owner.writes == 0);
	CHECK(w.phase() == wb::Phase::Editing && w.canApply());
	CHECK(w.snapshotInfo().revision == before.revision && value(w, root, true) == 99);
	CHECK(value(w, root) == 17 && w.info(root));
	unsigned applied = 0;
	CHECK(w.apply([&](const wb::ApplyResult& result) noexcept {
		++applied;
		CHECK(result.write.succeeded() && result.refresh.published);
	}));
	CHECK(rm::bindReadMany(w, h.backend, h.capability, ids).status == cl::Status::Busy);
	CHECK(rm::readMany(w, binding, [](const auto&) noexcept {
	      }).status == cl::Status::Busy);
	CHECK(rm::readFieldValue(w, h.field(1), [](const auto&) noexcept {
	      }).status == cl::Status::Busy);
	h.send(h.offered());
	w.process(h.now);
	CHECK(w.phase() == wb::Phase::Refreshing);
	CHECK(rm::readMany(w, binding, [](const auto&) noexcept {
	      }).status == cl::Status::Busy);
	h.pump();
	CHECK(applied == 1 && owner.value == 99 && owner.writes == 1);
	// An ordinary Workbench request occupies the same sole queue as optional reads.
	Harness<1> one;
	CHECK(one.workbench.readField(one.field(), [](const auto&) noexcept {
	}));
	CHECK(rm::readMany(one.workbench, one.many(), [](const auto&) noexcept {
	      }).status == cl::Status::QueueFull);
	one.pump();
}

static void requestAndServicePreserved() noexcept
{
	Harness h;
	auto& w = h.workbench;
	const auto command = w.bindCommand(0);
	CHECK(command && w.selectCommand(command.endpoint) == cl::Status::Ok);
	const auto root = w.editorChild(w.editorRoot().value, 0);
	CHECK(root && w.setScalar(root.value, std::uint32_t{123}) == rt::Status::Ok);
	const auto before = w.editorSnapshotInfo();
	CHECK(rm::readMany(w, h.many(), [](const auto& result) noexcept {
		CHECK(result.operation.succeeded());
	}));
	h.pump();
	CHECK(w.commandSelected() && w.editing() && value(w, root.value, true) == 123);
	CHECK(w.editorSnapshotInfo().revision == before.revision && owner.commands == 0);
	unsigned called = 0;
	CHECK(w.execute([&](const im::OperationResult& result) noexcept {
		++called;
		CHECK(result.succeeded());
	}));
	CHECK(rm::bindReadMany(w, h.backend, h.capability, ids).status == cl::Status::Busy);
	CHECK(rm::readFieldValue(w, h.field(1), [](const auto&) noexcept {
	      }).status == cl::Status::Busy);
	h.pump();
	CHECK(called == 1 && owner.commands == 1 && value(w, root.value, true) == 123);
	const auto service = w.bindService(0);
	CHECK(service && w.selectService(service.endpoint) == cl::Status::Ok);
	const auto request = w.editorChild(w.editorRoot().value, 0);
	CHECK(request && w.setScalar(request.value, std::uint32_t{456}) == rt::Status::Ok);
	CHECK(w.callService([](const wb::ServiceResult& result) noexcept {
		CHECK(result.published);
	}));
	h.pump();
	const auto response = w.serviceSnapshotInfo();
	const auto responseRoot = w.serviceRoot();
	CHECK(response.ready && responseRoot && w.serviceSelected());
	CHECK(rm::readFieldValue(w, h.field(), [](const auto& result) noexcept {
		CHECK(result.hasValue());
	}));
	CHECK(rm::readMany(w, h.many(), [](const auto&) noexcept {
	}));
	h.pump();
	CHECK(w.serviceSnapshotInfo().revision == response.revision &&
	      w.serviceScalar(w.serviceChild(responseRoot.value, 0).value));
	CHECK(w.serviceSelected() && value(w, request.value, true) == 456 && owner.calls == 1);
}

struct Reentry {
	Harness<>* h;
	im::NodeHandle root;
	rm::ReadManyBinding binding;
	unsigned moves = 0, destroys = 0, callbacks = 0;

	void check() noexcept
	{
		auto& w = h->workbench;
		const auto before = w.pending();
		CHECK(w.beginEdit() == rt::Status::Stale);
		CHECK(w.setScalar(root, std::uint32_t{7}) == rt::Status::Stale);
		w.cancelEdit();
		w.clearSelection();
		CHECK(w.selectField(h->field(1)) == cl::Status::Busy);
		CHECK(w.admit(h->metadata.schema, cl::Input{bytes}.first(64), 18) == cl::Status::Busy);
		CHECK(rm::bindReadMany(w, h->backend, h->capability, ids).status == cl::Status::Busy);
		CHECK(rm::readMany(w, binding, [](const auto&) noexcept {
		      }).status == cl::Status::Busy);
		CHECK(rm::readFieldValue(w, h->field(1), [](const auto&) noexcept {
		      }).status == cl::Status::Busy);
		w.process(h->now);
		CHECK(!w.nextPacket() && w.pending() == before);
		CHECK(w.editing() && w.dirty() && value(w, root, true) == 99 && value(w, root) == 17);
	}
};

struct ReentrantCallback {
	Reentry* probe;
	bool owned = false;

	explicit ReentrantCallback(Reentry& value) noexcept : probe(&value)
	{}

	ReentrantCallback(ReentrantCallback&& other) noexcept : probe(other.probe), owned(true)
	{
		++probe->moves;
		probe->check();
	}

	~ReentrantCallback()
	{
		if (owned) {
			++probe->destroys;
			probe->check();
		}
	}

	template<class Result>
	void operator()(const Result& result) noexcept
	{
		++probe->callbacks;
		CHECK(result.operation.succeeded());
		CHECK(rm::readMany(probe->h->workbench, probe->binding, [](const auto&) noexcept {
		      }).status == cl::Status::Busy);
	}
};

static void ownershipPublication() noexcept
{
	for (bool batch : {false, true}) {
		Harness h;
		const auto root = h.snapshot();
		CHECK(h.workbench.beginEdit() == rt::Status::Ok);
		CHECK(h.workbench.setScalar(root, std::uint32_t{99}) == rt::Status::Ok);
		Reentry probe{&h, root, h.many()};
		ReentrantCallback callback{probe};
		const auto accepted =
		    batch ? rm::readMany(h.workbench, probe.binding, std::move(callback))
		          : rm::readFieldValue(h.workbench, h.field(1), std::move(callback));
		CHECK(accepted && probe.moves > 0 && probe.destroys > 0 && probe.callbacks == 0);
		h.pump();
		CHECK(probe.callbacks == 1 && probe.destroys >= probe.moves);
		CHECK(h.workbench.canApply() && value(h.workbench, root, true) == 99);
		// Completion guard must restore false after callback invocation/destruction.
		CHECK(rm::readMany(h.workbench, probe.binding, [](const auto&) noexcept {
		}));
		h.pump();
	}
}

struct RevokeOnMove {
	Harness<>* h;
	bool close;

	RevokeOnMove(Harness<>& value, bool closing) noexcept : h(&value), close(closing)
	{}

	RevokeOnMove(RevokeOnMove&& other) noexcept : h(other.h), close(other.close)
	{
		if (close)
			h->workbench.close();
		else
			h->workbench.invalidate();
	}

	template<class Result>
	void operator()(const Result&) noexcept
	{
		CHECK(false);
	}
};

static void lifecycleAndRevocation() noexcept
{
	for (bool batch : {false, true})
		for (bool close : {false, true}) {
			Harness h;
			const auto binding = h.many();
			const auto field = h.field();
			const auto result =
			    batch ? rm::readMany(h.workbench, binding, RevokeOnMove{h, close})
			          : rm::readFieldValue(h.workbench, field, RevokeOnMove{h, close});
			const auto expected = close ? cl::Status::Closed : cl::Status::NotAdmitted;
			CHECK(result.status == expected && h.workbench.pending() == 0);
			CHECK(rm::bindReadMany(h.workbench, h.backend, {}, {}).status == expected);
			CHECK(rm::readMany(h.workbench, {}, [](const auto&) noexcept {
			      }).status == expected);
			CHECK(rm::readFieldValue(h.workbench, {}, [](const auto&) noexcept {
			      }).status == expected);
		}
	Harness h;
	const auto binding = h.many();
	unsigned callbacks = 0;
	CHECK(rm::readMany(
	    h.workbench, binding,
	    [&](const rm::ReadManyResult& result) noexcept {
		    ++callbacks;
		    CHECK(result.operation.status == cl::Status::Timeout);
		    CHECK(rm::bindReadMany(h.workbench, h.backend, {}, {}).status ==
		          cl::Status::RecoveryRequired);
		    CHECK(rm::readFieldValue(h.workbench, {}, [](const auto&) noexcept {
		          }).status == cl::Status::RecoveryRequired);
	    },
	    10));
	const auto packet = h.offered();
	h.send(packet, false);
	h.now += 10;
	h.workbench.process(h.now);
	CHECK(callbacks == 1 && h.workbench.recoveryRequired());
	CHECK(rm::readMany(h.workbench, {}, [](const auto&) noexcept {
	      }).status == cl::Status::RecoveryRequired);
	h.workbench.close();
	CHECK(rm::readMany(h.workbench, {}, [](const auto&) noexcept {
	      }).status == cl::Status::Closed);
}

struct DestroyRevocation {
	Harness<>* h;
	bool* armed;
	unsigned* callbacks;
	bool owned = false;

	DestroyRevocation(Harness<>& owner, bool& flag, unsigned& calls) noexcept
	    : h(&owner), armed(&flag), callbacks(&calls)
	{}

	DestroyRevocation(DestroyRevocation&& other) noexcept
	    : h(other.h), armed(other.armed), callbacks(other.callbacks), owned(true)
	{}

	~DestroyRevocation()
	{
		if (owned && *armed)
			h->workbench.close();
	}

	template<class Result>
	void operator()(const Result& result) noexcept
	{
		++*callbacks;
		CHECK(result.operation.succeeded());
	}
};

static void destructorRevocation() noexcept
{
	for (bool batch : {false, true}) {
		Harness h;
		const auto root = h.snapshot();
		CHECK(h.workbench.beginEdit() == rt::Status::Ok);
		CHECK(h.workbench.setScalar(root, std::uint32_t{99}) == rt::Status::Ok);
		const auto binding = h.many();
		unsigned callbacks = 0;
		bool armed = false;
		DestroyRevocation callback{h, armed, callbacks};
		CHECK(batch ? rm::readMany(h.workbench, binding, std::move(callback))
		            : rm::readFieldValue(h.workbench, h.field(1), std::move(callback)));
		armed = true;
		h.pump();
		CHECK(callbacks == 1 && !h.workbench.isCurrent() && !h.workbench.editing());
		CHECK(h.workbench.phase() == wb::Phase::Idle);
		CHECK(rm::readMany(h.workbench, binding, [](const auto&) noexcept {
		      }).status == cl::Status::Closed);
	}
}

template<class Binding>
struct ReplaceBindingOnMove {
	Binding* target;
	const Binding* replacement;
	unsigned* callbacks;

	ReplaceBindingOnMove(Binding& destination, const Binding& next, unsigned& calls) noexcept
	    : target(&destination), replacement(&next), callbacks(&calls)
	{}

	ReplaceBindingOnMove(ReplaceBindingOnMove&& other) noexcept
	    : target(other.target), replacement(other.replacement), callbacks(other.callbacks)
	{
		*target = *replacement;
	}

	void operator()(const rm::ReadManyResult& result) noexcept
	{
		++*callbacks;
		CHECK(result.operation.succeeded() && result.count == 2);
		CHECK(result.items[0].id == 1 && result.items[1].id == 2);
	}

	void operator()(const dy::DynamicResult& result) noexcept
	{
		++*callbacks;
		CHECK(result.hasValue() && result.value.get<std::uint64_t>().value == UINT64_MAX);
	}
};

static void bindingFrozenBeforeOwnership() noexcept
{
	Harness h;
	auto binding = h.many();
	const std::array<std::uint32_t, 1> otherIds{2};
	const auto replacement = rm::bindReadMany(h.workbench, h.backend, h.capability, otherIds);
	CHECK(replacement);
	unsigned callbacks = 0;
	CHECK(rm::readMany(h.workbench, binding,
	                   ReplaceBindingOnMove{binding, replacement.binding, callbacks}));
	CHECK(binding.entries().size() == 1);
	h.pump();
	CHECK(callbacks == 1);
	auto field = h.field(1);
	const auto otherField = h.field(2);
	CHECK(
	    rm::readFieldValue(h.workbench, field, ReplaceBindingOnMove{field, otherField, callbacks}));
	CHECK(field.id() == 2);
	h.pump();
	CHECK(callbacks == 2);
}

static unsigned functionCalls = 0;

static void manyFunction(const rm::ReadManyResult& result) noexcept
{
	CHECK(result.operation.succeeded() && result.count == 2);
	++functionCalls;
}

static void fieldFunction(const dy::DynamicResult& result) noexcept
{
	CHECK(result.operation.succeeded() && result.hasValue());
	++functionCalls;
}

static void functionReferences() noexcept
{
	Harness h;
	const auto binding = h.many();
	functionCalls = 0;
	CHECK(rm::readMany(h.workbench, binding, manyFunction));
	h.pump();
	CHECK(functionCalls == 1);
	CHECK(rm::readFieldValue(h.workbench, h.field(1), fieldFunction));
	h.pump();
	CHECK(functionCalls == 2);
}

int main()
{
	functionReferences();
	preservedDraftAndFifo();
	requestAndServicePreserved();
	ownershipPublication();
	lifecycleAndRevocation();
	destructorRevocation();
	bindingFrozenBeforeOwnership();
	std::printf("ReadMany Workbench: %u checks, 0 failures\n", checks);
	return 0;
}

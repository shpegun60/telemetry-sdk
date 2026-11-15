/* Shared canonical request editor and real Encoded/RPC Command acceptance. MIT. */
#include "CommandFixture.hpp"
#include <bit>
#include <cstring>
namespace cmd = command_fixture;
namespace cl = telemetry::client;
namespace rt = telemetry::runtime;
namespace im = telemetry::integration::inspector;
namespace wb = telemetry::integration::workbench;

static void initializationAndOverlap() noexcept
{
	cmd::Encoded<> f;
	const auto binding = f.inspector.bindCommand(0);
	CHECK(binding);
	const auto type = binding.endpoint.requestType();
	std::array<std::byte, 512> bytes{}, draft{};
	std::fill(bytes.begin(), bytes.end(), std::byte{0xa5});
	const auto size = binding.endpoint.requestBytes();
	CHECK(wb::RequestEditor::initialize(f.metadata.schema, type, std::span{bytes}.first(size)) ==
	      rt::Status::Ok);
	// Every value starts at zero except an enum, which starts at its first
	// named enumerator, so a fresh request holds only values its types name:
	// the request's three Mode values are Negative (-7, f9 ff); Wide's first,
	// Low, is zero.
	for (std::size_t i = 0; i < size; ++i)
		CHECK(bytes[i] == std::byte{0} ||
		      (bytes[i] == std::byte{0xf9} && i + 1 < size && bytes[i + 1] == std::byte{0xff}) ||
		      (bytes[i] == std::byte{0xff} && i > 0 && bytes[i - 1] == std::byte{0xf9}));
	CHECK(std::count(bytes.begin(), bytes.begin() + size, std::byte{0xf9}) == 3);
	CHECK(bytes[size] == std::byte{0xa5});
	const auto view = f.metadata.schema.view(type, rt::Input{bytes}.first(size));
	CHECK(view);
	const auto flag = view.value.member("flag");
	CHECK(flag && flag.value.get<bool>() && !flag.value.get<bool>().value);
	const auto floating = view.value.member("f32");
	CHECK(floating && floating.value.get<float>());
	CHECK(std::bit_cast<std::uint32_t>(floating.value.get<float>().value) == 0);
	const auto mode = view.value.member("nested").value.member("mode");
	CHECK(mode && mode.value.enumUnderlying<std::int16_t>());
	CHECK(mode.value.enumUnderlying<std::int16_t>().value == -7 &&
	      mode.value.enumName().value.known && mode.value.enumName().value.name == "Negative");
	std::fill(bytes.begin(), bytes.end(), std::byte{0xa5});
	const auto unchanged = bytes;
	CHECK(wb::RequestEditor::initialize(f.metadata.schema, type,
	                                    std::span{bytes}.first(size - 1)) == rt::Status::Capacity);
	CHECK(bytes == unchanged);
	CHECK(wb::RequestEditor::initialize(f.metadata.schema, UINT32_MAX, std::span{bytes}.first(8)) ==
	      rt::Status::TypeNotFound);
	CHECK(bytes == unchanged);
	CHECK(wb::RequestEditor::initialize(f.metadata.schema, 0, {}) == rt::Status::Ok);
	CHECK(!f.metadata.schema.view(0, {}));
	const auto rowsBefore = std::as_bytes(std::span{f.metadata.types});
	std::array<std::byte, sizeof(f.metadata.types)> metadataCopy{};
	std::copy(rowsBefore.begin(), rowsBefore.end(), metadataCopy.begin());
	CHECK(wb::RequestEditor::initialize(
	          f.metadata.schema, 8, std::as_writable_bytes(std::span{f.metadata.types}).first(8)) ==
	      rt::Status::Overlap);
	CHECK(std::equal(rowsBefore.begin(), rowsBefore.end(), metadataCopy.begin()));

	im::SnapshotStore source{std::span{bytes}.first(8)};
	CHECK(source.admit(f.metadata.schema, 17) == im::ModelStatus::Ok);
	CHECK(source.select(0, 8) == im::ModelStatus::Ok);
	const std::array<std::byte, 8> zero{};
	CHECK(source.publish(zero, source.admission(), source.revision()) == im::ModelStatus::Ok);
	std::array<std::byte, sizeof(source)> sourceCopy{};
	std::memcpy(sourceCopy.data(), &source, sizeof(source));
	auto alias = std::span{reinterpret_cast<std::byte*>(&source), std::size_t{8}};
	std::fill(draft.begin(), draft.end(), std::byte{0x5a});
	const auto draftCopy = draft;
	wb::RequestEditor aliasRequest{draft, alias};
	CHECK(aliasRequest.begin(source) == rt::Status::Overlap);
	CHECK(std::memcmp(&source, sourceCopy.data(), sizeof(source)) == 0 && draft == draftCopy);
	wb::RequestEditor aliasDraft{alias, draft};
	CHECK(aliasDraft.begin(source) == rt::Status::Overlap);
	CHECK(std::memcmp(&source, sourceCopy.data(), sizeof(source)) == 0 && draft == draftCopy);
	CHECK(source.select(0, 0) == im::ModelStatus::Ok);
	std::memcpy(sourceCopy.data(), &source, sizeof(source));
	CHECK(aliasRequest.begin(source) == rt::Status::Overlap);
	CHECK(std::memcmp(&source, sourceCopy.data(), sizeof(source)) == 0 && draft == draftCopy);
	CHECK(aliasDraft.begin(source) == rt::Status::Overlap);
	CHECK(std::memcmp(&source, sourceCopy.data(), sizeof(source)) == 0 && draft == draftCopy);
	CHECK(source.select(0, 8) == im::ModelStatus::Ok);
	CHECK(source.publish(zero, source.admission(), source.revision()) == im::ModelStatus::Ok);
	wb::RequestEditor healthy{draft, std::span{bytes}.first(8)};
	CHECK(healthy.begin(source) == rt::Status::Ok);
	CHECK(healthy.setScalar(source.root().value, std::uint64_t{42}) == rt::Status::Ok);
	CHECK(healthy.commit() && healthy.rearm() == rt::Status::Ok);
	CHECK(std::get<std::uint64_t>(healthy.scalar(source.root().value).value.value) == 42);
	const auto beforeReset = draft;
	CHECK(f.metadata.workspace.reset() == rt::Status::Ok);
	CHECK(!healthy.current() && !healthy.scalar(source.root().value));
	CHECK(wb::RequestEditor::initialize(f.metadata.schema, type, std::span{draft}.first(size)) ==
	      rt::Status::Stale);
	CHECK(draft == beforeReset);
}

template<class Fixture>
static void exactCommand() noexcept
{
	Fixture f;
	auto& model = f.inspector;
	const auto field = workbench_fixture::read(f);
	const auto fieldLeaf = workbench_fixture::child(model, field, 7);
	const auto originalField = model.snapshotInfo();
	CHECK(model.commands().size() == 5);
	(void)cmd::select(f);
	const auto root = cmd::root(model);
	CHECK(model.editorInfo(root).value.name == "Configure");
	CHECK(model.snapshotInfo().admission == originalField.admission && model.snapshotInfo().ready);
	CHECK(model.info(fieldLeaf) && !model.info(root));
	CHECK(model.editing() && !model.canApply() && !model.canEdit());
	CHECK(!cmd::scalar<bool>(model, cmd::child(model, root, 0)));
	CHECK(cmd::scalar<std::uint64_t>(model, cmd::child(model, root, 7)) == 0);
	CHECK(model.setScalar(fieldLeaf, std::uint64_t{99}) == rt::Status::Stale);
	const auto u64 = cmd::child(model, root, 7), i64 = cmd::child(model, root, 8);
	CHECK(model.setScalar(u64, std::uint32_t{9}) == rt::Status::TypeMismatch);
	CHECK(model.setScalar(u64, std::uint64_t{UINT64_MAX}) == rt::Status::Ok);
	CHECK(model.setScalar(i64, std::int64_t{INT64_MIN}) == rt::Status::Ok);
	const auto matrix = cmd::child(model, cmd::child(model, cmd::child(model, root, 13), 1), 0);
	CHECK(model.setScalar(matrix, std::uint32_t{31337}) == rt::Status::Ok);
	const auto signedEnum = cmd::child(model, cmd::child(model, root, 11), 2);
	CHECK(model.setScalar(signedEnum, std::int16_t{-123}) == rt::Status::Ok);
	CHECK(model.setScalar(cmd::child(model, root, 16), std::uint64_t{UINT64_MAX}) ==
	      rt::Status::Ok);
	const auto nan = std::bit_cast<float>(std::uint32_t{0xff812345});
	const auto negativeZero = std::bit_cast<double>(std::uint64_t{0x8000000000000000ULL});
	CHECK(model.setScalar(cmd::child(model, root, 9), nan) == rt::Status::Ok);
	CHECK(model.setScalar(cmd::child(model, root, 10), negativeZero) == rt::Status::Ok);
	unsigned callbacks = 0;
	const auto complete = [&](const im::OperationResult& result) noexcept {
		++callbacks;
		CHECK(result.succeeded() && result.executed() && !result.accepted());
		CHECK(result.operation == cl::Operation::Command);
	};
	CHECK(model.execute(complete));
	CHECK(model.phase() == wb::Phase::Executing && !model.canExecute() && !model.editing());
	CHECK(!model.root() && !model.info(fieldLeaf));
	CHECK(model.setScalar(u64, std::uint64_t{1}) == rt::Status::Stale);
	CHECK(model.selectCommand(model.bindCommand(2).endpoint) == cl::Status::Busy);
	CHECK(model.selectField(f.bind()) == cl::Status::Busy);
	CHECK(model.execute(complete).status == cl::Status::Busy);
	f.pump();
	CHECK(callbacks == 1 && cmd::device.commands == 1 && cmd::device.executed == 1 &&
	      cmd::device.reads == 1);
	CHECK(model.canExecute() && model.editing());
	CHECK(cmd::device.last.u64 == UINT64_MAX && cmd::device.last.i64 == INT64_MIN);
	CHECK(cmd::device.last.matrix[1][0] == 31337 &&
	      static_cast<std::int16_t>(cmd::device.last.nested.mode) == -123);
	CHECK(std::bit_cast<std::uint32_t>(cmd::device.last.f32) == 0xff812345);
	CHECK(std::bit_cast<std::uint64_t>(cmd::device.last.f64) == 0x8000000000000000ULL);
	CHECK(model.editorInfo(root) && cmd::scalar<std::uint64_t>(model, u64) == UINT64_MAX);
	// Stable argument handles and unchanged arguments permit an explicit second
	// command. There is no implicit FieldRead or a dirty requirement.
	CHECK(model.execute(complete));
	f.pump();
	CHECK(callbacks == 2 && cmd::device.commands == 2 && cmd::device.reads == 1);
	(void)cmd::select(f, 1);
	CHECK(!model.editorInfo(root));
	unsigned accepted = 0;
	CHECK(model.execute([&](const im::OperationResult& result) noexcept {
		++accepted;
		CHECK(result.succeeded() && result.accepted() && !result.executed());
	}));
	f.pump();
	CHECK(accepted == 1 && cmd::device.accepted == 1 && cmd::device.executed == 2);
	CHECK(model.canExecute());
	model.cancelEdit();
	CHECK(!model.commandSelected() && !model.canExecute());
}

template<class Fixture>
static void voidAndEmpty() noexcept
{
	Fixture f;
	auto& model = f.inspector;
	(void)cmd::select(f, 2);
	CHECK(model.editorSnapshotInfo().type == 0 && model.editorSnapshotInfo().selected);
	CHECK(!model.editorRoot() && model.canExecute());
	unsigned callbacks = 0;
	auto completed = [&](const im::OperationResult& r) noexcept {
		++callbacks;
		CHECK(r.executed());
	};
	CHECK(model.execute(completed));
	f.pump();
	CHECK(callbacks == 1);
	CHECK(model.execute(completed));
	f.pump();
	CHECK(callbacks == 2);
	(void)cmd::select(f, 3);
	CHECK(model.editorSnapshotInfo().type != 0 && model.editorSnapshotInfo().bytes == 0);
	const auto root = cmd::root(model);
	CHECK(model.editorChildCount(root).value == 0 &&
	      model.editorInfo(root).value.name == "EmptyArgs");
	CHECK(model.canExecute() && !model.dirty());
	CHECK(model.execute(completed));
	f.pump();
	CHECK(callbacks == 3 && cmd::device.commands == 3 && model.canExecute());
	CHECK(model.selectField(f.bind()) == cl::Status::Ok && !model.commandSelected());
	CHECK(!model.editorInfo(root));
}

static void refusalAndSupersession() noexcept
{
	{
		cmd::Encoded<1> f;
		unsigned callbacks = 0, unexpected = 0;
		im::NodeHandle arg;
		CHECK(f.inspector.readField(f.bind(), [&](const im::ReadResult& read) noexcept {
			CHECK(read.published);
			(void)cmd::select(f);
			arg = cmd::child(f.inspector, cmd::root(f.inspector), 7);
			CHECK(f.inspector.setScalar(arg, std::uint64_t{123}) == rt::Status::Ok);
			const auto refused = f.inspector.execute([&](const im::OperationResult&) noexcept {
				++unexpected;
			});
			CHECK(!refused && refused.status == cl::Status::QueueFull);
			CHECK(f.inspector.snapshotInfo().ready && f.inspector.canExecute());
			CHECK(cmd::scalar<std::uint64_t>(f.inspector, arg) == 123);
		}));
		f.pump();
		CHECK(unexpected == 0 && cmd::device.commands == 0);
		CHECK(f.inspector.execute([&](const im::OperationResult& r) noexcept {
			++callbacks;
			CHECK(r.executed());
		}));
		CHECK(!f.inspector.root());
		f.pump();
		CHECK(callbacks == 1 && cmd::device.commands == 1 && cmd::device.reads == 1 &&
		      cmd::device.last.u64 == 123);
	}
	{
		cmd::Encoded<> f;
		(void)workbench_fixture::read(f);
		unsigned reads = 0, commands = 0;
		CHECK(f.inspector.readField(f.bind(), [&](const im::ReadResult& result) noexcept {
			++reads;
			CHECK(result.operation.succeeded() && !result.published);
		}));
		(void)cmd::select(f);
		CHECK(f.inspector.execute([&](const im::OperationResult& r) noexcept {
			++commands;
			CHECK(r.executed());
		}));
		f.pump();
		CHECK(reads == 1 && commands == 1 && !f.inspector.root());
	}
	{
		cmd::Encoded<> f;
		(void)cmd::select(f);
		const auto root = cmd::root(f.inspector);
		const auto before = f.inspector.editorSnapshotInfo();
		const auto big = f.inspector.bindCommand(4);
		CHECK(big);
		CHECK(f.inspector.selectCommand(big.endpoint) == cl::Status::BufferTooSmall);
		CHECK(f.inspector.editorInfo(root) &&
		      f.inspector.editorSnapshotInfo().revision == before.revision);
		for (const auto timeout : {std::uint32_t{0}, cl::maxTimeoutMs + 1})
			CHECK(f.inspector
			          .execute(
			              [](const im::OperationResult&) noexcept {
			              },
			              timeout)
			          .status == cl::Status::InvalidArgument);
		void (*null)(const im::OperationResult&) noexcept = nullptr;
		CHECK(f.inspector.execute(null).status == cl::Status::InvalidArgument &&
		      f.inspector.canExecute());
		cmd::device.refuse = true;
		unsigned callbacks = 0;
		CHECK(f.inspector.execute([&](const im::OperationResult& r) noexcept {
			++callbacks;
			CHECK(!r.succeeded() && r.status == cl::Status::Ok &&
			      r.endpointStatus ==
			          static_cast<std::uint8_t>(telemetry::CommandResult::InvalidValue));
		}));
		f.pump();
		CHECK(callbacks == 1 && f.inspector.canExecute());
	}
}

static void phaseBoundaries() noexcept
{
	for (unsigned ending = 0; ending != 4; ++ending)
		for (unsigned phase = 0; phase != 3; ++phase) {
			cmd::Encoded<> f;
			auto& model = f.inspector;
			(void)cmd::select(f);
			unsigned callbacks = 0;
			cl::Status status = cl::Status::Ok;
			const auto submitted = model.execute(
			    [&](const im::OperationResult& result) noexcept {
				    ++callbacks;
				    status = result.status;
			    },
			    20);
			CHECK(submitted);
			cl::Packet packet{};
			std::size_t replyBytes = 0;
			if (phase)
				packet = f.offered();
			if (phase == 2)
				replyBytes = f.send(packet, false);
			if (ending == 0)
				f.now = 20;
			if (ending == 1)
				CHECK(model.cancel(submitted.handle));
			if (ending == 2)
				model.invalidate();
			if (ending == 3)
				model.close();
			model.process(f.now);
			if (ending == 2 && phase == 2) {
				CHECK(callbacks == 0 && !model.nextPacket());
				f.now = 20;
				model.process(f.now);
			}
			CHECK(callbacks == 1 && model.pending() == 0);
			CHECK(status == (phase == 2    ? cl::Status::OutcomeUnknown
			                 : ending == 0 ? cl::Status::Timeout
			                               : cl::Status::Cancelled));
			CHECK(model.recoveryRequired() == (phase == 2) &&
			      cmd::device.commands == (phase == 2 ? 1U : 0U));
			if (phase == 2) {
				CHECK(!model.commandSelected() && !model.canExecute());
				CHECK(!model.onReply(cl::Input{f.reply}.first(replyBytes), packet.session,
				                     packet.requestId));
				const auto refused = model.execute([](const im::OperationResult&) noexcept {
				});
				CHECK(refused.status ==
				      (ending == 3 ? cl::Status::Closed : cl::Status::RecoveryRequired));
			}
			model.process(100);
			CHECK(callbacks == 1 && cmd::device.commands == (phase == 2 ? 1U : 0U));
			if (ending == 0)
				std::printf(
				    "Workbench Command timeout phase=%s callbacks=%u commands=%u recovery=%u\n",
				    phase == 0   ? "queued"
				    : phase == 1 ? "offered"
				                 : "prepared",
				    callbacks, cmd::device.commands, unsigned(model.recoveryRequired()));
		}
}

static void stalePassiveAndReentrancy() noexcept
{
	{
		cmd::Encoded<> f;
		(void)cmd::select(f);
		const auto root = cmd::root(f.inspector);
		unsigned callbacks = 0;
		CHECK(f.inspector.execute([&](const im::OperationResult& r) noexcept {
			++callbacks;
			CHECK(r.executed());
		}));
		const auto packet = f.offered();
		const auto size = f.send(packet, false);
		CHECK(f.metadata.workspace.reset() == rt::Status::Ok);
		CHECK(!f.inspector.editorInfo(root) && !f.inspector.commandSelected() &&
		      !f.inspector.nextPacket());
		CHECK(
		    f.inspector.onReply(cl::Input{f.reply}.first(size), packet.session, packet.requestId));
		f.inspector.process(f.now);
		CHECK(callbacks == 1 && cmd::device.commands == 1 && !f.inspector.canExecute());
	}
	{
		cmd::Encoded<> f;
		(void)cmd::select(f);
		unsigned callbacks = 0;
		CHECK(f.inspector.execute([&](const im::OperationResult& r) noexcept {
			++callbacks;
			CHECK(r.executed());
			CHECK(f.inspector
			          .execute([](const im::OperationResult&) noexcept {
			          })
			          .status == cl::Status::Busy);
			CHECK(f.inspector.selectCommand(f.inspector.bindCommand(2).endpoint) ==
			      cl::Status::Busy);
			f.inspector.close();
			f.inspector.process(f.now);
			CHECK(!f.inspector.drained());
		}));
		f.pump();
		f.inspector.process(f.now);
		CHECK(callbacks == 1 && f.inspector.drained());
		CHECK(f.inspector
		          .execute([](const im::OperationResult&) noexcept {
		          })
		          .status == cl::Status::Closed);
	}
}

static void rpcBoundaries() noexcept
{
	for (const bool finalAuthorization : {false, true}) {
		cmd::Rpc f;
		(void)cmd::select(f);
		unsigned callbacks = 0;
		cl::Status status = cl::Status::Ok;
		CHECK(f.inspector.execute(
		    [&](const im::OperationResult& r) noexcept {
			    ++callbacks;
			    status = r.status;
		    },
		    100));
		for (unsigned i = 0; i != 40; ++i) {
			auto packet = f.inspector.nextPacket();
			if (!packet) {
				(void)f.pool.process(++f.now);
				f.inspector.process(f.now);
				continue;
			}
			if (packet.mayExecute && finalAuthorization) {
				CHECK(f.inspector.prepareHandoff(packet.session, packet.requestId));
				break;
			}
			CHECK(!packet.mayExecute);
			std::copy(packet.bytes.begin(), packet.bytes.end(), f.port.begin());
			CHECK(f.inspector.prepareHandoff(packet.session, packet.requestId));
			CHECK(f.inspector.onSendAccepted(packet.session, packet.requestId));
			const auto reply =
			    f.pool.handle(f.session, cl::Input{f.port}.first(packet.bytes.size()), f.reply);
			CHECK(reply.status == cmd::rpc::RpcSessionStatus::Ok &&
			      reply.reply.status == resource::Status::Ok);
			if (reply.ticket.serial)
				CHECK(f.pool.replyAccepted(f.session, reply.ticket) ==
				      cmd::rpc::RpcSessionStatus::Ok);
			CHECK(f.inspector.onReply(cl::Input{f.reply}.first(reply.reply.written), packet.session,
			                          packet.requestId));
			if (!finalAuthorization)
				break;
		}
		CHECK(cmd::device.commands == 0 &&
		      f.mailbox.state() == telemetry::protocol::rpc::RpcState::Receiving);
		f.inspector.invalidate();
		CHECK(!f.inspector.nextPacket());
		f.inspector.process(f.now);
		if (finalAuthorization) {
			CHECK(callbacks == 0);
			f.now = 100;
			f.inspector.process(f.now);
		}
		CHECK(callbacks == 1 &&
		      status == (finalAuthorization ? cl::Status::OutcomeUnknown : cl::Status::Cancelled));
		CHECK(!f.inspector.confirmQuiescence({1, false, true, true}));
		CHECK(f.pool.release(f.session) == cmd::rpc::RpcSessionStatus::Ok);
		(void)f.pool.process(f.now);
		CHECK(f.mailbox.state() == telemetry::protocol::rpc::RpcState::Idle);
		CHECK(f.inspector.confirmQuiescence({1, true, true, true}));
		CHECK(cmd::device.commands == 0);
	}
}

static void explicitReadFromCommandCompletion() noexcept
{
	for (const bool selectFirst : {false, true}) {
		cmd::Encoded<2> f;
		(void)cmd::select(f);
		const auto arg = cmd::child(f.inspector, cmd::root(f.inspector), 7);
		CHECK(f.inspector.setScalar(arg, std::uint64_t{75}) == rt::Status::Ok);
		unsigned commands = 0, reads = 0;
		CHECK(f.inspector.execute([&](const im::OperationResult& result) noexcept {
			++commands;
			CHECK(result.executed() && cmd::device.reads == 0);
			if (selectFirst)
				CHECK(f.inspector.selectField(f.bind()) == cl::Status::Ok);
			CHECK(f.inspector.readField(f.bind(), [&](const im::ReadResult& read) noexcept {
				++reads;
				CHECK(read.operation.succeeded() && read.published);
			}));
			CHECK(!f.inspector.commandSelected() && !f.inspector.editing());
			CHECK(result.executed());
			CHECK(f.inspector
			          .execute([](const im::OperationResult&) noexcept {
			          })
			          .status == cl::Status::Busy);
			CHECK(f.inspector
			          .apply([](const wb::ApplyResult&) noexcept {
			          })
			          .status == cl::Status::Busy);
		}));
		f.pump();
		CHECK(commands == 1 && reads == 1 && cmd::device.commands == 1 && cmd::device.reads == 1);
		const auto snapshot = f.inspector.root();
		CHECK(snapshot && f.inspector.canEdit());
		const auto value =
		    f.inspector.scalar(workbench_fixture::child(f.inspector, snapshot.value, 7));
		CHECK(value && std::get<std::uint64_t>(value.value.value) == 75);
	}
}

template<class Result>
struct OwnershipState {
	void* context;
	void (*moving)(void*) noexcept = nullptr;
	void (*destroying)(void*) noexcept = nullptr;
	unsigned moves = 0, destructions = 0, callbacks = 0;
};

// User-defined ownership operations are not completion callbacks. Probe them
// separately, including the live target's destruction on a refused submission.
template<class Result>
struct OwnershipProbe {
	OwnershipState<Result>* state;

	explicit OwnershipProbe(OwnershipState<Result>& value) noexcept : state(&value)
	{}

	OwnershipProbe(const OwnershipProbe&) = delete;

	OwnershipProbe(OwnershipProbe&& other) noexcept : state(std::exchange(other.state, nullptr))
	{
		if (state && state->moves++ == 0 && state->moving)
			state->moving(state->context);
	}

	~OwnershipProbe()
	{
		if (state) {
			++state->destructions;
			if (state->destroying)
				state->destroying(state->context);
		}
	}

	void operator()(const Result&) noexcept
	{
		++state->callbacks;
	}
};

static void ownershipPublication() noexcept
{
	{
		cmd::Encoded<> f;
		unsigned reads = 0;
		CHECK(f.inspector.readField(f.bind(), [&](const im::ReadResult&) noexcept {
			++reads;
		}));
		(void)cmd::select(f);
		const auto arg = cmd::child(f.inspector, cmd::root(f.inspector), 7);
		CHECK(f.inspector.setScalar(arg, std::uint64_t{73}) == rt::Status::Ok);

		struct Context {
			cmd::Encoded<>* fixture;
			im::NodeHandle arg;
			unsigned* reads;
		} context{&f, arg, &reads};

		OwnershipState<im::OperationResult> state{
		    &context, +[](void* pointer) noexcept {
			    auto& c = *static_cast<Context*>(pointer);
			    auto& model = c.fixture->inspector;
			    CHECK(model.selectCommand(model.bindCommand(2).endpoint) == cl::Status::Busy);
			    CHECK(model.selectField(c.fixture->bind()) == cl::Status::Busy);
			    CHECK(model.setScalar(c.arg, std::uint64_t{999}) == rt::Status::Stale);
			    CHECK(model
			              .readField(c.fixture->bind(),
			                         [](const im::ReadResult&) noexcept {
			                         })
			              .status == cl::Status::Busy);
			    CHECK(model
			              .execute([](const im::OperationResult&) noexcept {
			              })
			              .status == cl::Status::Busy);
			    CHECK(!model.canExecute() && !model.canApply() && !model.canEdit() &&
			          !model.drained());
			    model.cancelEdit();
			    model.clearSelection();
			    model.process(c.fixture->now);
			    CHECK(!model.nextPacket() && *c.reads == 0 && cmd::device.commands == 0);
			    CHECK(!model.prepareHandoff(17, 1) && !model.onSendAccepted(17, 1) &&
			          !model.onReply({}, 17, 1));
			    CHECK(!model.confirmQuiescence({1, true, true, true}));
		    }};
		CHECK(f.inspector.execute(OwnershipProbe{state}));
		CHECK(state.moves > 0 && state.callbacks == 0 &&
		      f.inspector.phase() == wb::Phase::Executing);
		f.pump();
		CHECK(state.callbacks == 1 && state.destructions == 1 && reads == 1);
		CHECK(cmd::device.commands == 1 && cmd::device.executed == 1 && cmd::device.last.u64 == 73);
	}
	{
		cmd::Encoded<> f;
		(void)cmd::select(f);
		OwnershipState<im::OperationResult> state{
		    &f, +[](void* pointer) noexcept {
			    auto& fixture = *static_cast<cmd::Encoded<>*>(pointer);
			    fixture.inspector.close();
			    fixture.inspector.process(fixture.now);
			    CHECK(!fixture.inspector.nextPacket() && !fixture.inspector.drained());
		    }};
		CHECK(f.inspector.execute(OwnershipProbe{state}).status == cl::Status::Closed);
		f.inspector.process(f.now);
		CHECK(state.callbacks == 0 && state.destructions == 1 && cmd::device.commands == 0 &&
		      f.inspector.drained());
	}
	{
		cmd::Encoded<1> f;
		unsigned destructions = 0;
		CHECK(f.inspector.readField(f.bind(), [&](const im::ReadResult&) noexcept {
			(void)cmd::select(f);
			const auto arg = cmd::child(f.inspector, cmd::root(f.inspector), 7);
			CHECK(f.inspector.setScalar(arg, std::uint64_t{123}) == rt::Status::Ok);
			OwnershipState<im::OperationResult> state{
			    &f, nullptr, +[](void* pointer) noexcept {
				    auto& fixture = *static_cast<cmd::Encoded<1>*>(pointer);
				    CHECK(fixture.inspector.selectCommand(
				              fixture.inspector.bindCommand(2).endpoint) == cl::Status::Busy);
				    fixture.inspector.cancelEdit();
				    CHECK(!fixture.inspector.nextPacket() && !fixture.inspector.drained());
			    }};
			CHECK(f.inspector.execute(OwnershipProbe{state}).status == cl::Status::QueueFull);
			CHECK(state.callbacks == 0 && state.destructions == 1);
			destructions = state.destructions;
			CHECK(f.inspector.canExecute() && cmd::scalar<std::uint64_t>(f.inspector, arg) == 123);
		}));
		f.pump();
		CHECK(destructions == 1 && cmd::device.commands == 0);
	}
	{
		cmd::Encoded<> f;
		const auto root = workbench_fixture::read(f);
		const auto arg = workbench_fixture::child(f.inspector, root, 7);
		CHECK(f.inspector.beginEdit() == rt::Status::Ok);
		CHECK(f.inspector.setScalar(arg, std::uint64_t{74}) == rt::Status::Ok);
		OwnershipState<wb::ApplyResult> state{
		    &f,
		    +[](void* pointer) noexcept {
			    auto& fixture = *static_cast<cmd::Encoded<>*>(pointer);
			    CHECK(fixture.inspector.selectCommand(fixture.inspector.bindCommand(2).endpoint) ==
			          cl::Status::Busy);
			    fixture.inspector.cancelEdit();
			    CHECK(!fixture.inspector.nextPacket() && !fixture.inspector.canApply());
		    },
		    +[](void* pointer) noexcept {
			    auto& fixture = *static_cast<cmd::Encoded<>*>(pointer);
			    CHECK(fixture.inspector.selectCommand(fixture.inspector.bindCommand(2).endpoint) ==
			          cl::Status::Busy);
			    CHECK(!fixture.inspector.nextPacket() && !fixture.inspector.drained());
		    }};
		CHECK(f.inspector.apply(OwnershipProbe{state}));
		f.pump();
		CHECK(state.callbacks == 1 && state.destructions == 1 && cmd::device.packet.u64 == 74 &&
		      cmd::device.commands == 0);
	}
}

static unsigned functionCalls = 0;

static void functionCompletion(const im::OperationResult& result) noexcept
{
	CHECK(result.executed());
	++functionCalls;
}

static void functionReferences() noexcept
{
	cmd::Encoded<> f;
	(void)cmd::select(f, 2);
	functionCalls = 0;
	CHECK(f.inspector.execute(functionCompletion));
	f.pump();
	CHECK(functionCalls == 1 && cmd::device.commands == 1);
}

int main()
{
	functionReferences();
	initializationAndOverlap();
	exactCommand<cmd::Encoded<>>();
	exactCommand<cmd::Rpc>();
	voidAndEmpty<cmd::Encoded<>>();
	voidAndEmpty<cmd::Rpc>();
	refusalAndSupersession();
	phaseBoundaries();
	stalePassiveAndReentrancy();
	rpcBoundaries();
	explicitReadFromCommandCompletion();
	ownershipPublication();
	std::printf("Workbench Command: %u checks, 0 failures\n", inspector_fixture::checks);
	return 0;
}

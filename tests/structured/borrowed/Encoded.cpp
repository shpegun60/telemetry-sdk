/* Request-only storage, full-byte large responses and checked boundaries. MIT. */
#include "Fixture.hpp"
#include "Check.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

using namespace borrowed_fixture;
using borrowed_test::check;

namespace {
template<std::size_t N>
void fields()
{
	static Owner<Blob<N>> owner{};
	static std::array<std::byte, N + 4> output{};
	static std::array<std::byte, N> input{};
	static std::array<std::byte, N + 32> memory{};
	owner.reads = owner.writes = 0;
	fill(owner.value, 17);
	for (std::size_t i = 0; i < N; ++i)
		input[i] = static_cast<std::byte>((i * 37u + 23u) & 255u);
	ts::FieldTable table{ts::field<&Owner<Blob<N>>::get, &Owner<Blob<N>>::set>("Writable", owner),
	                     ts::field<&Owner<Blob<N>>::get>("Readonly", owner)};
	ts::Model model{
	    ts::group("large", ts::field<&Owner<Blob<N>>::get, &Owner<Blob<N>>::set>("Writable", owner),
	              ts::field<&Owner<Blob<N>>::get>("Readonly", owner))};
	const auto& entry = table[0];
	check(entry.readScratchBytes == 0 && entry.writeScratchBytes == N &&
	      table[1].readScratchBytes == 0 && table[1].writeScratchBytes == 0);
	check(model.maxFieldScratch() == N && model.maxScratch() == N);
	ts::Workspace empty{std::span<std::byte>{}};
	output.fill(std::byte{0xcd});
	auto result = entry.readEncoded(output, empty);
	check(result.dispatch == DS::Ok && result.written == N &&
	      pattern<N>(std::span{output}.first(N), 17) && output[N] == std::byte{0xcd} &&
	      output.back() == std::byte{0xcd} && owner.reads == 1 && empty.used() == 0);
	result = model.fieldIndex().readEncoded(1u, output, empty);
	check(result.dispatch == DS::Ok && result.written == N && owner.reads == 2 &&
	      empty.used() == 0);
	result = entry.readEncoded(std::span{output}.first(N - 1), empty);
	check(result.dispatch == DS::BufferTooSmall && result.written == 0 && owner.reads == 2 &&
	      pattern<N>(std::span{output}.first(N), 17));
	check(entry.writeEncoded(std::span{input}.first(N - 1), empty).dispatch == DS::InvalidPayload &&
	      owner.writes == 0);
	check(entry.writeEncoded(input, empty).dispatch == DS::WorkspaceTooSmall && owner.writes == 0 &&
	      empty.used() == 0);
	ts::Workspace workspace{std::span{memory}.subspan(1)};
	check(entry.writeEncoded(std::span{memory}.subspan(1, N), workspace).dispatch ==
	          DS::InvalidPayload &&
	      owner.writes == 0 && workspace.used() == 0);
	{
		auto outer = workspace.reserve<std::uint32_t>();
		auto* marker = outer.constructFrom([]() noexcept {
			return std::uint32_t{0xabcdef01};
		});
		const auto prior = workspace.used();
		const auto written = entry.writeEncoded(input, workspace);
		check(marker != nullptr && written.dispatch == DS::Ok &&
		      written.endpointStatus == WR::Applied && owner.writes == 1 &&
		      workspace.used() == prior && *marker == 0xabcdef01 &&
		      std::equal(owner.value.bytes.begin(), owner.value.bytes.end(), input.begin(),
		                 [](std::uint8_t a, std::byte b) {
			                 return a == std::to_integer<std::uint8_t>(b);
		                 }));
	}
	check(workspace.used() == 0);
	const auto before = owner.reads;
	auto native = std::as_writable_bytes(std::span<Blob<N>>{std::addressof(owner.value), 1});
	result = entry.readEncoded(native, empty);
	check(result.dispatch == DS::InvalidPayload && result.written == 0 &&
	      owner.reads == before + 1 && pattern<N>(native, 23));
	check(table[1].writeEncoded(input, empty).endpointStatus == WR::ReadOnly && owner.writes == 1);
	// Twelve checks per response size. The setter still copies explicitly.
}

template<class R>
unsigned seed(const R& request) noexcept
{
	if constexpr (std::is_same_v<R, Request>)
		return request.seed;
	else
		return request.bytes.front();
}

// Counted responder retains the entire payload while request-only scratch is exercised.
// API: call().
template<std::size_t N, class R>
struct Responder {
	Blob<N> response{};
	unsigned calls = 0, observed = 0;

	const Blob<N>& call(const R& request) noexcept
	{
		++calls;
		observed = seed(request);
		return response;
	}
};

// Zero-request responder retains the entire borrowed payload without DTO request storage.
// API: call().
template<std::size_t N>
struct Responder<N, void> {
	Blob<N> response{};
	unsigned calls = 0, observed = 0;

	const Blob<N>& call() noexcept
	{
		++calls;
		return response;
	}
};

template<std::size_t N, class R>
void service()
{
	using OwnerType = Responder<N, R>;
	constexpr auto requestWire = ts::wireSize<R>;
	constexpr auto required = [] {
		if constexpr (std::is_void_v<R>)
			return std::size_t{0};
		else if constexpr (sizeof(R) <= ts::maxLocalObjectBytes)
			return std::size_t{0};
		else
			return sizeof(R) + alignof(R) - 1;
	}();
	static OwnerType owner{};
	static std::array<std::byte, requestWire + 1> input{};
	static std::array<std::byte, N + 4> output{};
	static std::array<std::byte, N + required + 32> memory{};
	static std::array<std::byte, (N > requestWire ? N : requestWire) + 8> shared{};
	owner.calls = owner.observed = 0;
	fill(owner.response, 41);
	input.fill(std::byte{5});
	if constexpr (std::is_same_v<R, Request>)
		input[0] = std::byte{1};
	const auto request = std::span{input}.first(requestWire);
	const auto definition = ts::service<&OwnerType::call>("Borrowed", owner);
	ts::ServiceTable table{definition};
	const auto& entry = table[0];
	check(entry.scratchBytes == required && entry.responseWireBytes == N &&
	      entry.requestWireBytes == requestWire);
	ts::Workspace empty{std::span<std::byte>{}};
	output.fill(std::byte{0xcd});
	auto result = entry.callEncoded(request, std::span{output}.first(N - 1), empty);
	check(result.dispatch == DS::BufferTooSmall && result.written == 0 && owner.calls == 0 &&
	      output.front() == std::byte{0xcd});
	result = entry.callEncoded(input, output, empty);
	check(result.dispatch == DS::InvalidPayload && result.written == 0 && owner.calls == 0 &&
	      output.front() == std::byte{0xcd});
	ts::Workspace shortWorkspace{std::span{memory}.first(required == 0 ? 0 : required - 1)};
	result = entry.callEncoded(request, output, shortWorkspace);
	check(result.dispatch == (required == 0 ? DS::Ok : DS::WorkspaceTooSmall) &&
	      owner.calls == (required == 0 ? 1u : 0u) && shortWorkspace.used() == 0);
	ts::Workspace workspace{std::span{memory}.subspan(1)};
	unsigned before = owner.calls;
	{
		auto outer = workspace.reserve<std::uint32_t>();
		auto* marker = outer.constructFrom([]() noexcept {
			return std::uint32_t{0x13579bdf};
		});
		const auto prior = workspace.used();
		result = entry.callEncoded(request, output, workspace);
		check(marker != nullptr && result.dispatch == DS::Ok && result.endpointStatus == SS::Ok &&
		      result.written == N && owner.calls == before + 1 &&
		      pattern<N>(std::span{output}.first(N), 41) && output[N] == std::byte{0xcd} &&
		      workspace.used() == prior && *marker == 0x13579bdf &&
		      owner.observed == (std::is_void_v<R> ? 0u : 5u));
	}
	check(workspace.used() == 0);
	before = owner.calls;
	result = entry.callEncoded(request, output, empty);
	check(result.dispatch == (required == 0 ? DS::Ok : DS::WorkspaceTooSmall) &&
	      owner.calls == before + (required == 0 ? 1u : 0u) && empty.used() == 0);
	shared.fill(std::byte{0xa5});
	std::copy(request.begin(), request.end(), shared.begin() + 3);
	const auto tailBefore = shared[N + 2];
	before = owner.calls;
	result = entry.callEncoded(std::span{shared}.subspan(3, requestWire),
	                           std::span{shared}.subspan(2, N), workspace);
	check(result.dispatch == DS::Ok && result.written == N && owner.calls == before + 1 &&
	      pattern<N>(std::span{shared}.subspan(2, N), 41) && shared.front() == std::byte{0xa5} &&
	      shared[N + 2] == tailBefore && workspace.used() == 0);
	before = owner.calls;
	result = entry.callEncoded(request, std::span{memory}.subspan(1, N), workspace);
	check(result.dispatch == (required == 0 ? DS::Ok : DS::InvalidPayload) &&
	      owner.calls == before + (required == 0 ? 1u : 0u) && workspace.used() == 0);
	if constexpr (std::is_same_v<R, Request>) {
		input[0] = std::byte{2};
		before = owner.calls;
		result = entry.callEncoded(request, output, workspace);
		check(result.dispatch == DS::InvalidPayload && result.written == 0 &&
		      owner.calls == before && workspace.used() == 0);
		input[0] = std::byte{1};
	} else {
		check(entry.scratchBytes == required && output[N] == std::byte{0xcd});
	}
	before = owner.calls;
	auto native = std::as_writable_bytes(std::span<Blob<N>>{std::addressof(owner.response), 1});
	result = entry.callEncoded(request, native, workspace);
	check(result.dispatch == DS::InvalidPayload && result.written == 0 &&
	      owner.calls == before + 1 && pattern<N>(native, 41) && workspace.used() == 0);
	if constexpr (std::is_void_v<R>) {
		check(definition.call().valueOrNull() == std::addressof(owner.response));
	} else {
		static R nativeRequest{};
		check(definition.call(nativeRequest).valueOrNull() == std::addressof(owner.response));
	}
	// Twelve checks per size/request pair, independent of local-storage budget.
}

template<class Field, class Service, class Bind, class Reset>
void slots(Field field, Service service, Bind bind, Reset reset)
{
	ts::FieldTable fields{field};
	ts::ServiceTable services{service};
	std::array<std::byte, 2> input{std::byte{1}, std::byte{7}};
	std::array<std::byte, 5> output{};
	std::array<std::byte, 128> scratch{};
	ts::Workspace workspace{scratch};
	auto pair = [&](const Config* expected) {
		output.fill(std::byte{0xcd});
		const auto f = fields[0].readEncoded(output, workspace);
		const bool fieldOkay =
		    expected
		        ? f.dispatch == DS::Ok && f.written == 5 && configBytes(output, *expected)
		        : f.dispatch == DS::Unavailable && f.written == 0 && output[0] == std::byte{0xcd};
		output.fill(std::byte{0xcd});
		const auto s = services[0].callEncoded(input, output, workspace);
		check(fieldOkay &&
		      (expected ? s.dispatch == DS::Ok && s.endpointStatus == SS::Ok && s.written == 5 &&
		                      configBytes(output, *expected)
		                : s.dispatch == DS::Unavailable && s.written == 0 &&
		                      output[0] == std::byte{0xcd}) &&
		      workspace.used() == 0);
	};
	pair(nullptr);
	bind(false);
	pair(&first.value);
	bind(true);
	pair(&second.value);
	reset();
	pair(nullptr);
}

void slotMatrix()
{
	ts::OwnerSlot<Device> owner;
	slots(
	    ts::field<&Device::get>("Owner", owner), ts::service<&Device::call>("Owner", owner),
	    [&](bool alt) {
		    owner.bind(alt ? second : first);
	    },
	    [&] {
		    owner.reset();
	    });
	ts::FunctionSlot<const Config&() noexcept> functionGet;
	ts::FunctionSlot<const Config&(const Request&) noexcept> functionCall;
	slots(
	    ts::field("Function", functionGet), ts::service("Function", functionCall),
	    [&](bool alt) {
		    functionGet.bind(alt ? &getSecond : &getFirst);
		    functionCall.bind(alt ? &callSecond : &callFirst);
	    },
	    [&] {
		    functionGet.reset();
		    functionCall.reset();
	    });
	ts::ContextFunctionSlot<const Config&() noexcept> contextGetSlot;
	ts::ContextFunctionSlot<const Config&(const Request&) noexcept> contextCallSlot;
	slots(
	    ts::field("Context", contextGetSlot), ts::service("Context", contextCallSlot),
	    [&](bool alt) {
		    contextGetSlot.bind(&contextGet, alt ? &second : &first);
		    contextCallSlot.bind(&contextCall, alt ? &second : &first);
	    },
	    [&] {
		    contextGetSlot.reset();
		    contextCallSlot.reset();
	    });
	auto getter = []() noexcept -> const Config& {
		return first.get();
	};
	auto alternateGetter = []() noexcept -> const Config& {
		return second.get();
	};
	auto caller = [](const Request& q) noexcept -> const Config& {
		return first.call(q);
	};
	auto alternateCaller = [](const Request& q) noexcept -> const Config& {
		return second.call(q);
	};
	ts::DelegateRefSlot<const Config&() noexcept> referenceGet;
	ts::DelegateRefSlot<const Config&(const Request&) noexcept> referenceCall;
	slots(
	    ts::field("Reference", referenceGet), ts::service("Reference", referenceCall),
	    [&](bool alt) {
		    if (alt) {
			    referenceGet.bind(alternateGetter);
			    referenceCall.bind(alternateCaller);
		    } else {
			    referenceGet.bind(getter);
			    referenceCall.bind(caller);
		    }
	    },
	    [&] {
		    referenceGet.reset();
		    referenceCall.reset();
	    });
	ts::DelegateSlot<const Config&() noexcept> delegateGet;
	ts::DelegateSlot<const Config&(const Request&) noexcept> delegateCall;
	slots(
	    ts::field("Delegate", delegateGet), ts::service("Delegate", delegateCall),
	    [&](bool alt) {
		    auto* selected = alt ? &second : &first;
		    delegateGet.bind([selected]() noexcept -> const Config& {
			    return selected->get();
		    });
		    delegateCall.bind([selected](const Request& q) noexcept -> const Config& {
			    return selected->call(q);
		    });
	    },
	    [&] {
		    delegateGet.reset();
		    delegateCall.reset();
	    });
}

void overlap()
{
	struct Storage {
		std::array<std::byte, 8> prefix;
		Config value;
		std::array<std::byte, 16> suffix;
	};

	Storage storage{};
	storage.value = {0x12345678, true};
	auto getter = [&]() noexcept -> const Config& {
		return storage.value;
	};
	auto caller = [&](const Request&) noexcept -> const Config& {
		return storage.value;
	};
	ts::FieldTable fields{ts::field("Value", getter)};
	ts::ServiceTable services{ts::service("Value", caller)};
	std::array<std::byte, 64> scratch{};
	ts::Workspace workspace{scratch};
	const std::array<std::byte, 2> input{std::byte{1}, std::byte{7}};
	auto bytes = std::as_writable_bytes(std::span<Storage>{&storage, 1});
	const auto beforeValue = storage.value;
	std::array<std::byte, sizeof(Storage)> before{};
	std::memcpy(before.data(), std::addressof(storage), before.size());
	auto f = fields[0].readEncoded(bytes.subspan(offsetof(Storage, value), 5), workspace);
	check(f.dispatch == DS::InvalidPayload && f.written == 0 &&
	      std::memcmp(std::addressof(storage), before.data(), before.size()) == 0);
	// Tail padding is part of the complete native object even though absent on wire.
	static_assert(sizeof(Config) == 8 && ts::wireSize<Config> == 5);
	f = fields[0].readEncoded(bytes.subspan(offsetof(Storage, value) + 5, 5), workspace);
	check(f.dispatch == DS::InvalidPayload && f.written == 0 &&
	      std::memcmp(std::addressof(storage), before.data(), before.size()) == 0);
	const auto s =
	    services[0].callEncoded(input, bytes.subspan(offsetof(Storage, value) + 5, 5), workspace);
	check(s.dispatch == DS::InvalidPayload && s.written == 0 &&
	      std::memcmp(std::addressof(storage), before.data(), before.size()) == 0);
	// The output tail may contain native data: only the written prefix is tested.
	f = fields[0].readEncoded(bytes, workspace);
	check(f.dispatch == DS::Ok && f.written == 5 && configBytes(bytes.first(5), storage.value) &&
	      storage.value == beforeValue);
	auto s2 = services[0].callEncoded(input, bytes, workspace);
	check(s2.dispatch == DS::Ok && s2.written == 5 && configBytes(bytes.first(5), storage.value) &&
	      storage.value == beforeValue);
}

// Nested Service consumes a large Request from the shared Workspace.
// API: call().
struct Inner {
	Config value{73, true};
	unsigned calls = 0;

	const Config& call(const Blob<65>&) noexcept
	{
		++calls;
		return value;
	}
};

// Outer Service checks nested leases restore Workspace before returning its Request view.
// API: call().
struct Outer {
	ts::Workspace* workspace;
	const ts::ServiceEntry* inner;
	unsigned calls = 0;
	bool restored = false;

	const Request& call(const Request& request) noexcept
	{
		++calls;
		const auto prior = workspace->used();
		const std::array<std::byte, 65> input{};
		std::array<std::byte, 5> output{};
		const auto result = inner->callEncoded(input, output, *workspace);
		restored = result.dispatch == DS::Ok && result.written == 5 && workspace->used() == prior;
		return request; // The decoded request lease/local remains alive during encoding.
	}
};

void nested()
{
	std::array<std::byte, 256> scratch{};
	ts::Workspace workspace{std::span{scratch}.subspan(1)};
	Inner inner;
	ts::ServiceTable innerTable{ts::service<&Inner::call>("Inner", inner)};
	Outer outer{&workspace, &innerTable[0]};
	ts::ServiceTable outerTable{ts::service<&Outer::call>("Outer", outer)};
	check(innerTable[0].scratchBytes == 65 &&
	      outerTable[0].scratchBytes ==
	          (sizeof(Request) <= ts::maxLocalObjectBytes ? 0 : ts::scratchBytes<Request>));
	const std::array<std::byte, 2> input{std::byte{1}, std::byte{9}};
	std::array<std::byte, 2> output{};
	{
		auto lease = workspace.reserve<std::uint32_t>();
		auto* marker = lease.constructFrom([]() noexcept {
			return std::uint32_t{0xdeadbeef};
		});
		const auto prior = workspace.used();
		const auto result = outerTable[0].callEncoded(input, output, workspace);
		check(result.dispatch == DS::Ok && result.written == 2 && output == input &&
		      outer.restored && outer.calls == 1 && inner.calls == 1);
		check(marker != nullptr && *marker == 0xdeadbeef && workspace.used() == prior);
	}
	check(workspace.used() == 0);
	const std::array<std::byte, 2> invalid{std::byte{2}, std::byte{9}};
	check(outerTable[0].callEncoded(invalid, output, workspace).dispatch == DS::InvalidPayload &&
	      outer.calls == 1 && inner.calls == 1 && output == input);
}

// Custom Field binding counts target snapshots before availability and borrowed invocation.
// API: snapshot(), available(), invoke().
struct SnapshotGetter {
	using Signature = const Config&() noexcept;
	unsigned* snapshots;
	const Config* target;

	const Config* snapshot() const noexcept
	{
		++*snapshots;
		return target;
	}

	static bool available(const Config* target) noexcept
	{
		return target != nullptr;
	}

	static const Config& invoke(const Config* target) noexcept
	{
		return *target;
	}
};

// Custom Service binding counts target snapshots before availability and borrowed invocation.
// API: snapshot(), available(), invoke().
struct SnapshotCaller {
	using Signature = const Config&(const Request&) noexcept;
	unsigned* snapshots;
	const Config* target;

	const Config* snapshot() const noexcept
	{
		++*snapshots;
		return target;
	}

	static bool available(const Config* target) noexcept
	{
		return target != nullptr;
	}

	static const Config& invoke(const Config* target, const Request&) noexcept
	{
		return *target;
	}
};

void selectedOnce()
{
	unsigned fieldSnapshots = 0, serviceSnapshots = 0;
	ts::FieldDefinition field{"Snapshot",
	                          SnapshotGetter{&fieldSnapshots, std::addressof(first.value)}};
	ts::ServiceDefinition service{"Snapshot",
	                              SnapshotCaller{&serviceSnapshots, std::addressof(first.value)}};
	ts::FieldTable fields{field};
	ts::ServiceTable services{service};
	std::array<std::byte, 16> scratch{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, 5> output{};
	const std::array<std::byte, 2> input{std::byte{1}, std::byte{7}};
	const auto read = fields[0].readEncoded(output, workspace);
	check(read.dispatch == DS::Ok && read.written == 5 && fieldSnapshots == 1 &&
	      configBytes(output, first.value));
	const auto call = services[0].callEncoded(input, output, workspace);
	check(call.dispatch == DS::Ok && call.written == 5 && serviceSnapshots == 1 &&
	      configBytes(output, first.value) && workspace.used() == 0);
}
} // namespace

int main()
{
	borrowed_test::start();
	fields<4096>();
	fields<65536>();
	service<4096, void>();
	service<4096, Request>();
	service<4096, Blob<16>>();
	service<4096, Blob<32>>();
	service<4096, Blob<64>>();
	service<4096, Blob<4096>>();
	service<65536, void>();
	service<65536, Request>();
	service<65536, Blob<16>>();
	service<65536, Blob<32>>();
	service<65536, Blob<64>>();
	service<65536, Blob<4096>>();
	slotMatrix();
	overlap();
	nested();
	selectedOnce();
	return borrowed_test::finish(); // 24 + 144 + 20 + 5 + 5 + 2 + 1 = 201.
}

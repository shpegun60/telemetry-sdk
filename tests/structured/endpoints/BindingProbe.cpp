/*
 * @file BindingProbe.cpp
 * @brief Field/Command callback forms, late binding and preflight ordering.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/model/Model.hpp>
#include <algorithm>
#include <cstdio>

namespace ts = telemetry;
using WR = telemetry::WriteResult;
using CR = telemetry::CommandResult;
#define CHECK(...)                                                                                 \
	do {                                                                                           \
		if (!(__VA_ARGS__)) {                                                                      \
			std::printf("line %d\n", __LINE__);                                                    \
			return 1;                                                                              \
		}                                                                                          \
	} while (false)

namespace fixture {
struct Value {
	std::uint32_t number;
	bool enabled;
};

// Mutable owner exposes const/nonconst getters, setter and Command binding variants.
// API: get(), getConst(), set(), call().
struct Device {
	Value value{19, true};
	int reads = 0, writes = 0, calls = 0;

	Value get() noexcept
	{
		++reads;
		return value;
	}

	Value getConst() const noexcept
	{
		return value;
	}

	WR set(const Value& next) noexcept
	{
		++writes;
		value = next;
		return WR::Applied;
	}

	CR call(Value next) noexcept
	{
		++calls;
		value = next;
		return CR::Executed;
	}
};

struct Prefix {
	std::uint32_t padding = 0;
};

struct Derived : Prefix, Device {};

inline Device device;

Value get() noexcept
{
	return device.get();
}

WR set(Value value) noexcept
{
	return device.set(value);
}

CR call(const Value& value) noexcept
{
	return device.call(value);
}

Value query(const Value& value) noexcept
{
	return value;
}

Value contextGet(void* p) noexcept
{
	return static_cast<Device*>(p)->get();
}

WR contextSet(void* p, const Value& value) noexcept
{
	return static_cast<Device*>(p)->set(value);
}

CR contextCall(void* p, const Value& value) noexcept
{
	return static_cast<Device*>(p)->call(value);
}

// Counts the binding operation separately from callback effects. Each route
// must observe one coherent target, even when the binding is replaceable.
// Custom binding observes the snapshot/availability/invoke sequence before callbacks.
// API: snapshot(), available(), invoke().
template<class SignatureT, auto Function>
struct Snapshot {
	using Signature = SignatureT;
	int* count;

	int snapshot() const noexcept
	{
		return ++*count;
	}

	static bool available(int) noexcept
	{
		return true;
	}

	template<class... Args>
	static auto invoke(int, Args&&... args) noexcept
	{
		return Function(std::forward<Args>(args)...);
	}
};
} // namespace fixture

template<class Getter, class Setter, class Command>
bool roundtrip(Getter& getter, Setter& setter, Command& command)
{
	using fixture::Value;
	ts::FieldTable fields{ts::field("value", getter, setter)};
	ts::CommandTable commands{ts::command("update", command)};
	// The same bindings in one group of a runtime tree, for encoded access.
	ts::Model model{
	    ts::group("test", ts::field("value", getter, setter), ts::command("update", command))};
	std::array<std::byte, 64> scratch{}, output{};
	ts::Workspace workspace{scratch};
	std::array<std::byte, ts::wireSize<Value>> bytes{};
	if (ts::encode(Value{23, false}, bytes) != ts::CodecStatus::Ok)
		return false;
	if (!fields.template read<0>() || fields.template write<0>(Value{21, true}) != WR::Applied ||
	    commands.template call<0>(Value{22, true}) != CR::Executed)
		return false;
	if (model.fieldIndex().writeEncoded(0, bytes, workspace).endpointStatus != WR::Applied ||
	    model.commandIndex().executeEncoded(0, bytes, workspace).endpointStatus != CR::Executed)
		return false;
	const auto result = model.fieldIndex().readEncoded(0, output, workspace);
	return result.dispatch == ts::DispatchStatus::Ok && result.written == bytes.size() &&
	       std::equal(bytes.begin(), bytes.end(), output.begin()) && workspace.used() == 0;
}

template<class Getter, class Setter, class Command>
bool empty(Getter& getter, Setter& setter, Command& command)
{
	using fixture::Value;
	ts::FieldTable fields{ts::field("value", getter, setter)};
	ts::CommandTable commands{ts::command("update", command)};
	ts::Model model{
	    ts::group("test", ts::field("value", getter, setter), ts::command("update", command))};
	const int before = fixture::device.reads + fixture::device.writes + fixture::device.calls;
	std::array<std::byte, 64> scratch{};
	std::array<std::byte, ts::wireSize<Value>> bytes{};
	ts::Workspace workspace{scratch};
	if (fields.template read<0>() || fields.template write<0>(Value{}) != WR::Unavailable ||
	    commands.template call<0>(Value{}) != CR::Unavailable)
		return false;
	if (model.fieldIndex().readEncoded(0, bytes, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    model.fieldIndex().writeEncoded(0, bytes, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    model.commandIndex().executeEncoded(0, bytes, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable)
		return false;
	// Malformed values are rejected before availability, as with Services.
	bytes.back() = std::byte{2};
	if (model.fieldIndex().writeEncoded(0, bytes, workspace).dispatch !=
	        ts::DispatchStatus::InvalidPayload ||
	    model.commandIndex().executeEncoded(0, bytes, workspace).dispatch !=
	        ts::DispatchStatus::InvalidPayload)
		return false;
	return before == fixture::device.reads + fixture::device.writes + fixture::device.calls &&
	       workspace.used() == 0;
}

bool wrappedFunctions()
{
	using namespace fixture;
	auto refGet = std::ref(get);
	auto refSet = std::ref(set);
	auto refCall = std::ref(call);
	if (!roundtrip(refGet, refSet, refCall))
		return false;
	auto constGet = std::cref(get);
	auto constSet = std::cref(set);
	auto constCall = std::cref(call);
	if (!roundtrip(constGet, constSet, constCall))
		return false;

	auto getter = &get;
	auto setter = &set;
	auto command = &call;
	auto service = &query;
	ts::FieldTable fields{ts::field("value", std::ref(getter), std::cref(setter))};
	ts::CommandTable commands{ts::command("update", std::cref(command))};
	ts::ServiceTable services{ts::service("query", std::ref(service)),
	                          ts::service("function", std::cref(query))};
	std::array<std::byte, 64> storage{};
	ts::Workspace workspace{storage};
	std::array<std::byte, ts::wireSize<Value>> bytes{}, output{};
	const Value expected{31, true};
	if (ts::encode(expected, bytes) != ts::CodecStatus::Ok)
		return false;
	getter = nullptr;
	setter = nullptr;
	command = nullptr;
	service = nullptr;
	if (fields.read<0>() || fields.write<0>(expected) != WR::Unavailable ||
	    commands.call<0>(expected) != CR::Unavailable ||
	    services.call<0>(expected).status() != ts::ServiceStatus::Unavailable)
		return false;
	if (fields.data()[0].readEncoded(output, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    fields.data()[0].writeEncoded(bytes, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    commands.data()[0].executeEncoded(bytes, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    services.data()[0].callEncoded(bytes, output, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable)
		return false;
	getter = &get;
	setter = &set;
	command = &call;
	service = &query;
	if (fields.write<0>(expected) != WR::Applied || !fields.read<0>() ||
	    commands.call<0>(expected) != CR::Executed ||
	    services.call<0>(expected).status() != ts::ServiceStatus::Ok)
		return false;
	for (const auto& entry : services) {
		const auto result = entry.callEncoded(bytes, output, workspace);
		if (result.dispatch != ts::DispatchStatus::Ok ||
		    result.endpointStatus != ts::ServiceStatus::Ok || result.written != bytes.size() ||
		    output != bytes)
			return false;
	}
	return fields.data()[0].readEncoded(output, workspace).dispatch == ts::DispatchStatus::Ok &&
	       output == bytes && workspace.used() == 0;
}

int main()
{
	using namespace fixture;
	constexpr auto fixed = ts::field<&get, &set>("value");
	constexpr auto action = ts::command<&call>("action");
	CHECK(fixed.write(Value{20, true}) == WR::Applied && fixed.read()->number == 20);
	CHECK(action.call(Value{21, false}) == CR::Executed);
	CHECK(roundtrip(get, set, call)); // Bare functions must decay to stored pointers.
	CHECK(wrappedFunctions());
	auto g = &get;
	auto s = &set;
	auto c = &call;
	CHECK(roundtrip(g, s, c));
	g = nullptr;
	s = nullptr;
	c = nullptr;
	CHECK(empty(g, s, c));

	const auto lambdas = ts::field(
	    "inline",
	    []() noexcept {
		    return get();
	    },
	    [](Value value) noexcept {
		    return set(value);
	    });
	const auto plusLambda = ts::command(
	    "inline", +[](const Value& value) noexcept {
		    return call(value);
	    });
	CHECK(lambdas.read() && lambdas.write(Value{22, true}) == WR::Applied);
	CHECK(plusLambda.call(Value{23, true}) == CR::Executed);
	auto closureGet = [&device = device]() noexcept {
		return device.get();
	};
	auto closureSet = [&device = device](const Value& value) noexcept {
		return device.set(value);
	};
	auto closureCall = [&device = device](const Value& value) noexcept {
		return device.call(value);
	};
	CHECK(roundtrip(closureGet, closureSet, closureCall));
	auto refGet = std::ref(closureGet);
	auto refSet = std::ref(closureSet);
	auto refCall = std::ref(closureCall);
	CHECK(roundtrip(refGet, refSet, refCall));

	Derived derived;
	const auto member = ts::field<&Device::get, &Device::set>("member", derived);
	const auto memberCommand = ts::command<&Device::call>("member", std::ref(derived));
	CHECK(member.write(Value{24, false}) == WR::Applied && member.read()->number == 24);
	CHECK(memberCommand.call(Value{25, false}) == CR::Executed && derived.value.number == 25);
	CHECK(ts::field<&Device::getConst>("const", std::cref(derived)).read()->number == 25);

	telemetry::OwnerSlot<Device> owner;
	ts::FieldTable ownerFields{ts::field<&Device::get, &Device::set>("owner", owner)};
	ts::CommandTable ownerCommands{ts::command<&Device::call>("owner", owner)};
	CHECK(!ownerFields.read<0>() && ownerFields.write<0>(Value{}) == WR::Unavailable);
	CHECK(ownerCommands.call<0>(Value{}) == CR::Unavailable);
	owner.bind(device);
	CHECK(ownerFields.read<0>() && ownerFields.write<0>(Value{26, true}) == WR::Applied);
	owner.bind(derived);
	CHECK(ownerCommands.call<0>(Value{27, true}) == CR::Executed && derived.value.number == 27);
	owner.reset();
	CHECK(!ownerFields.read<0>() && ownerCommands.call<0>(Value{}) == CR::Unavailable);

	telemetry::FunctionSlot<Value() noexcept> functionGet;
	telemetry::FunctionSlot<WR(Value) noexcept> functionSet;
	telemetry::FunctionSlot<CR(const Value&) noexcept> functionCall;
	CHECK(empty(functionGet, functionSet, functionCall));
	functionGet.bind(&get);
	functionSet.bind(&set);
	functionCall.bind(&call);
	CHECK(roundtrip(functionGet, functionSet, functionCall));
	functionGet.reset();
	functionSet.reset();
	functionCall.reset();
	CHECK(empty(functionGet, functionSet, functionCall));

	telemetry::ContextFunctionSlot<Value() noexcept> contextGetter;
	telemetry::ContextFunctionSlot<WR(const Value&) noexcept> contextSetter;
	telemetry::ContextFunctionSlot<CR(const Value&) noexcept> contextCommand;
	CHECK(empty(contextGetter, contextSetter, contextCommand));
	contextGetter.bind(&contextGet, &device);
	contextSetter.bind(&contextSet, &device);
	contextCommand.bind(&contextCall, &device);
	CHECK(roundtrip(contextGetter, contextSetter, contextCommand));

	telemetry::DelegateRefSlot<Value() noexcept> refGetter;
	telemetry::DelegateRefSlot<WR(const Value&) noexcept> refSetter;
	telemetry::DelegateRefSlot<CR(const Value&) noexcept> refCommand;
	CHECK(empty(refGetter, refSetter, refCommand));
	refGetter.bind(closureGet);
	refSetter.bind(closureSet);
	refCommand.bind(closureCall);
	CHECK(roundtrip(refGetter, refSetter, refCommand));

	telemetry::DelegateSlot<Value() noexcept> ownedGetter;
	telemetry::DelegateSlot<WR(const Value&) noexcept> ownedSetter;
	telemetry::DelegateSlot<CR(const Value&) noexcept> ownedCommand;
	CHECK(empty(ownedGetter, ownedSetter, ownedCommand));
	ownedGetter.bind(closureGet);
	ownedSetter.bind(closureSet);
	ownedCommand.bind(closureCall);
	CHECK(roundtrip(ownedGetter, ownedSetter, ownedCommand));

	int snapshots = 0;
	ts::FieldDefinition countedField{"snapshots", Snapshot<Value() noexcept, &get>{&snapshots},
	                                 Snapshot<WR(Value) noexcept, &set>{&snapshots}};
	ts::CommandDefinition countedCommand{"update",
	                                     Snapshot<CR(const Value&) noexcept, &call>{&snapshots}};
	ts::FieldTable countedFields{countedField};
	ts::CommandTable countedCommands{countedCommand};
	ts::Model model{ts::group("test", countedField, countedCommand)};
	std::array<std::byte, 64> scratch{};
	std::array<std::byte, ts::wireSize<Value>> bytes{};
	ts::Workspace workspace{scratch};
	CHECK(model.fieldIndex().readEncoded(0, bytes, workspace).dispatch == ts::DispatchStatus::Ok &&
	      snapshots == 1);
	CHECK(model.fieldIndex().writeEncoded(0, bytes, workspace).dispatch == ts::DispatchStatus::Ok &&
	      snapshots == 2);
	CHECK(model.commandIndex().executeEncoded(0, bytes, workspace).dispatch ==
	          ts::DispatchStatus::Ok &&
	      snapshots == 3);
	CHECK(countedFields.read<0>() && snapshots == 4);
	CHECK(countedFields.write<0>(Value{}) == WR::Applied && snapshots == 5);
	CHECK(countedCommands.call<0>(Value{}) == CR::Executed && snapshots == 6);
	return 0;
}

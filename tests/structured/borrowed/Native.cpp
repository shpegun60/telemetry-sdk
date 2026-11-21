/* Exact types, native traversal and every standard slot family. MIT. */
#include "Fixture.hpp"
#include "Check.hpp"
#include <concepts>
#include <functional>
#include <limits>
#include <optional>

using namespace borrowed_fixture;
using borrowed_test::check;
#if !defined(_WIN32)
// Undefined weak C++ targets need external linkage; references are not C ABI.
const Config& absentBorrowedField() noexcept __attribute__((weak));
const Config& absentBorrowedService(const Request&) noexcept __attribute__((weak));
#endif
namespace {
struct Empty {
	friend bool operator==(const Empty&, const Empty&) = default;
};

template<class T>
void fieldType(T initial, T alternate)
{
	Owner<T> owner{initial};
	const auto definition = ts::field<&Owner<T>::get, &Owner<T>::set>("Value", owner);
	using Definition = decltype(definition);
	static_assert(Definition::borrowsValue);
	static_assert(std::same_as<typename Definition::Value, T>);
	static_assert(std::same_as<typename Definition::ReadResult, ts::BorrowedValue<T>>);
	static_assert(std::same_as<decltype(definition.read()), ts::BorrowedValue<T>>);
	auto view = definition.read();
	check(view && view.valueOrNull() == std::addressof(owner.value) && view.value() == initial);
	ts::FieldTable table{definition};
	ts::Model model{ts::group("group", definition)};
	check(table.template read<0>().valueOrNull() == std::addressof(owner.value) &&
	      model.fields().template readBorrowed<T>(0u).valueOrNull() == std::addressof(owner.value));
	auto copied = definition.template readAs<T>();
	static_assert(std::same_as<decltype(copied), std::optional<T>>);
	owner.value = alternate;
	check(copied && *copied == initial && view.value() == alternate);
	check(definition.write(initial) == WR::Applied && owner.writes == 1 && view.value() == initial);
	const auto before = owner.reads;
	unsigned visitors = 0;
	table.forEach([&](const auto& selected) {
		++visitors;
		check(std::addressof(selected) == std::addressof(table.template get<0>()));
	});
	check(visitors == 1 && owner.reads == before && table.visit(0u, [](const auto&) {
	}));
}

template<class Field, class Service, class Bind, class Reset>
void slots(Field& field, Service& service, Bind bind, Reset reset)
{
	const Request request{true, 7};
	unsigned before = first.reads + first.calls + second.reads + second.calls;
	check(!field.read() && service.call(request).status() == SS::Unavailable &&
	      before == first.reads + first.calls + second.reads + second.calls);
	bind(false);
	auto old = field.read();
	auto answer = service.call(request);
	check(old.valueOrNull() == std::addressof(first.value) &&
	      answer.valueOrNull() == std::addressof(first.value));
	bind(true);
	check(field.read().valueOrNull() == std::addressof(second.value) &&
	      service.call(request).valueOrNull() == std::addressof(second.value) &&
	      old.valueOrNull() == std::addressof(first.value));
	reset();
	before = first.reads + first.calls + second.reads + second.calls;
	check(!field.read() && !service.call(request) &&
	      before == first.reads + first.calls + second.reads + second.calls);
}

// Stable callable lvalue forwards a borrowed Field getter to its selected owner.
// API: operator().
struct Getter {
	Device* owner;

	const Config& operator()() const noexcept
	{
		return owner->get();
	}
};

// Stable callable lvalue forwards a borrowed Service request to its selected owner.
// API: operator().
struct Caller {
	Device* owner;

	const Config& operator()(const Request& q) const noexcept
	{
		return owner->call(q);
	}
};

struct Prefix {
	std::uint64_t prefix = 0;
};

struct Derived : Prefix, Device {};

// Setter takes Config by value while the matching getter borrows existing Config.
// API: get(), set().
struct ValueSetter {
	Config value{17, true};

	const Config& get() const noexcept
	{
		return value;
	}

	WR set(Config next) noexcept
	{
		value = next;
		return WR::Applied;
	}
};

// Custom Field binding exposes one counted snapshot, availability test and invocation.
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

	static bool available(const Config* p) noexcept
	{
		return p != nullptr;
	}

	static const Config& invoke(const Config* p) noexcept
	{
		return *p;
	}
};

// Custom Service binding exposes one counted snapshot, availability test and invocation.
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

	static bool available(const Config* p) noexcept
	{
		return p != nullptr;
	}

	static const Config& invoke(const Config* p, const Request&) noexcept
	{
		return *p;
	}
};
} // namespace

int main()
{
	borrowed_test::start();
	fieldType(false, true);
	fieldType(std::uint8_t{17}, std::uint8_t{255});
	fieldType(std::uint16_t{17}, std::uint16_t{65535});
	fieldType(std::uint32_t{17}, std::uint32_t{0xffffffff});
	fieldType(std::int8_t{-17}, std::numeric_limits<std::int8_t>::lowest());
	fieldType(std::int16_t{-17}, std::numeric_limits<std::int16_t>::lowest());
	fieldType(std::int32_t{-17}, std::numeric_limits<std::int32_t>::lowest());
	fieldType(std::int64_t{-17}, std::numeric_limits<std::int64_t>::lowest());
	fieldType(std::uint64_t{17}, std::numeric_limits<std::uint64_t>::max());
	fieldType(1.5f, -2.0f);
	fieldType(1.5, -2.0);
	fieldType(Mode::Off, static_cast<Mode>(-2));
	fieldType(std::array<std::uint16_t, 3>{1, 2, 3}, std::array<std::uint16_t, 3>{4, 5, 6});
	fieldType(Config{17, true}, Config{23, false});
	fieldType(Empty{}, Empty{});
	fieldType(std::array<std::uint8_t, 0>{}, std::array<std::uint8_t, 0>{});
	// Sixteen Field shapes, six conditions each (one is a traversal callback).
	const Request request{true, 1};
	auto freeField = ts::field<&getFirst>("Free");
	auto freeService = ts::service<&callFirst>("Free");
	static_assert(decltype(freeService)::borrowsResponse);
	static_assert(
	    std::same_as<typename decltype(freeService)::Result, ts::BorrowedServiceResult<Config>>);
	check(freeField.read().valueOrNull() == std::addressof(first.value) &&
	      freeService.call(request).valueOrNull() == std::addressof(first.value));
	auto runtimeField = ts::field("Runtime", &getFirst);
	auto runtimeService = ts::service("Runtime", &callFirst);
	check(runtimeField.read().valueOrNull() == std::addressof(first.value) &&
	      runtimeService.call(request).valueOrNull() == std::addressof(first.value));
	auto lambdaField = ts::field("Lambda", []() noexcept -> const Config& {
		return first.value;
	});
	auto lambdaService = ts::service("Lambda", [](const Request&) noexcept -> const Config& {
		return first.value;
	});
	check(lambdaField.read().valueOrNull() == std::addressof(first.value) &&
	      lambdaService.call(request).valueOrNull() == std::addressof(first.value));
	Getter getter{&first};
	Caller caller{&first};
	auto callableField = ts::field("Callable", getter);
	auto callableService = ts::service("Callable", caller);
	check(callableField.read().valueOrNull() == std::addressof(first.value) &&
	      callableService.call(request).valueOrNull() == std::addressof(first.value));
	Derived derived;
	auto derivedField = ts::field<&Device::get>("Derived", derived);
	auto derivedService = ts::service<&Device::call>("Derived", derived);
	check(derivedField.read().valueOrNull() == std::addressof(derived.value) &&
	      derivedService.call(request).valueOrNull() == std::addressof(derived.value));
	unsigned fieldSnapshots = 0, serviceSnapshots = 0;
	ts::FieldDefinition snapshotField{"Snapshot", SnapshotGetter{&fieldSnapshots, &first.value}};
	ts::ServiceDefinition snapshotService{"Snapshot",
	                                      SnapshotCaller{&serviceSnapshots, &first.value}};
	check(snapshotField.read().valueOrNull() == std::addressof(first.value) &&
	      fieldSnapshots == 1 &&
	      snapshotService.call(request).valueOrNull() == std::addressof(first.value) &&
	      serviceSnapshots == 1);
	ValueSetter byValue;
	const auto byValueField =
	    ts::field<&ValueSetter::get, &ValueSetter::set>("Value setter", byValue);
	check(byValueField.write(Config{23, false}) == WR::Applied &&
	      byValueField.read().valueOrNull() == std::addressof(byValue.value) &&
	      byValue.value == Config{23, false});
	ts::FieldTable byValueTable{byValueField};
	ts::Model byValueModel{ts::group("values", byValueField)};
	auto tableCopy = byValueTable.readAs<Config>(0u);
	auto catalogCopy = byValueModel.fields().readAs<Config>(0u);
	byValue.value = {31, true};
	check(tableCopy && catalogCopy && *tableCopy == Config{23, false} &&
	      *catalogCopy == Config{23, false} && byValueTable.read<0>().value() == Config{31, true});
	ts::OwnerSlot<Device> owner;
	auto ownerField = ts::field<&Device::get>("Owner", owner);
	auto ownerService = ts::service<&Device::call>("Owner", owner);
	slots(
	    ownerField, ownerService,
	    [&](bool alt) {
		    owner.bind(alt ? second : first);
	    },
	    [&] {
		    owner.reset();
	    });
	ts::FunctionSlot<const Config&() noexcept> functionGet;
	ts::FunctionSlot<const Config&(const Request&) noexcept> functionCall;
	auto functionField = ts::field("Function", functionGet);
	auto functionService = ts::service("Function", functionCall);
	slots(
	    functionField, functionService,
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
	auto contextField = ts::field("Context", contextGetSlot);
	auto contextService = ts::service("Context", contextCallSlot);
	slots(
	    contextField, contextService,
	    [&](bool alt) {
		    contextGetSlot.bind(&contextGet, alt ? &second : &first);
		    contextCallSlot.bind(&contextCall, alt ? &second : &first);
	    },
	    [&] {
		    contextGetSlot.reset();
		    contextCallSlot.reset();
	    });
	Getter alternateGetter{&second};
	Caller alternateCaller{&second};
	ts::DelegateRefSlot<const Config&() noexcept> referenceGet;
	ts::DelegateRefSlot<const Config&(const Request&) noexcept> referenceCall;
	auto referenceField = ts::field("Reference", referenceGet);
	auto referenceService = ts::service("Reference", referenceCall);
	slots(
	    referenceField, referenceService,
	    [&](bool alt) {
		    referenceGet.bind(alt ? alternateGetter : getter);
		    referenceCall.bind(alt ? alternateCaller : caller);
	    },
	    [&] {
		    referenceGet.reset();
		    referenceCall.reset();
	    });
	ts::DelegateSlot<const Config&() noexcept> delegateGet;
	ts::DelegateSlot<const Config&(const Request&) noexcept> delegateCall;
	auto delegateField = ts::field("Delegate", delegateGet);
	auto delegateService = ts::service("Delegate", delegateCall);
	slots(
	    delegateField, delegateService,
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
#if !defined(_WIN32)
	check(!ts::field<&absentBorrowedField>("Weak").read());
	check(ts::service<&absentBorrowedService>("Weak").call(request).status() == SS::Unavailable);
#endif
	return borrowed_test::finish(); // 125 on PE host; 127 on ELF host.
}

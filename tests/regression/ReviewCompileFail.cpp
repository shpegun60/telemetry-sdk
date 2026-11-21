// Invalid definitions must fail for their actual contract, not a secondary error.
// Authors: Ruslan Kovtun (shpegun60), codexAi. License: MIT.
// Preserves diagnostic-specific refusals found during earlier identifier and borrowed-slot reviews.
// The selected cases isolate narrowing, copied mutable arguments and temporary owner or callable conversions.

#include <telemetry/Telemetry.hpp>
#include <functional>
#include <memory>
using namespace telemetry;

float reviewRead() noexcept
{
	return 1.f;
}

CommandResult run() noexcept
{
	return CommandResult::Executed;
}

// Native owner control for exact read and command bindings.
// Public methods:
// - read(): Return stored value.
// - run(): Return command status.
struct Owner {
	float value = 2.f;

	float read() const noexcept
	{
		return value;
	}

	CommandResult run() const noexcept
	{
		return CommandResult::Executed;
	}
} owner;

// Conversion holder whose owner reference cannot extend the holder lifetime.
// Public methods:
// - operator const Owner&(): Expose borrowed subobject.
struct Holder {
	Owner value;

	operator const Owner&() const noexcept
	{
		return value;
	}
};

// Conversion proxy returning a fresh owner instead of an actual borrowed object.
// Public methods:
// - operator Owner(): Manufacture temporary owner.
struct ValueHolder {
	operator Owner() const noexcept
	{
		return Owner{};
	}
} proxy;

// Small callable used as the stable-target lifetime control.
// Public methods:
// - operator(): Return fixed value.
struct Read {
	float operator()() const noexcept
	{
		return 1.f;
	}
} reader;

// Conversion holder whose callable reference expires with the holder.
// Public methods:
// - operator const Read&(): Expose borrowed subobject.
struct ReadHolder {
	Read value;

	operator const Read&() const noexcept
	{
		return value;
	}
} readProxy;

Owner* pointer = &owner;
std::unique_ptr<Owner> smart;
std::reference_wrapper<Owner> wrapped{owner};

#if CASE == 12
constexpr auto bad = detail::makeId(0, 65536);
#elif CASE == 13
constexpr auto bad = detail::makeId(UINT64_MAX, 0);
#elif CASE == 14
constexpr auto bad = detail::makeId(-1, 0);
#elif CASE == 15
auto bad = detail::makeId(0.0, 1);
#elif CASE == 19
void bad()
{
	DelegateSlot<void(int&) noexcept> slot;
	slot.bind([](auto value) noexcept {
		(void)value;
	});
}
#elif CASE == 20
void bad()
{
	DelegateRefSlot<void(int&) noexcept> slot;
	auto f = [](auto value) noexcept {
		(void)value;
	};
	slot.bind(f);
}
#elif CASE == 21
void bad()
{
	DelegateRefSlot<float() noexcept> slot;
	slot.bind<&Owner::read, const Owner>(Holder{});
}
#elif CASE == 22
void bad()
{
	DelegateRefSlot<float() noexcept> slot;
	slot.bind<const Read>(ReadHolder{});
}
#elif CASE == 23
void bad()
{
	DelegateRefSlot<float() noexcept> slot;
	slot.bind<&Owner::read>(wrapped);
}
#elif CASE == 24
void bad()
{
	DelegateRefSlot<float() noexcept> slot;
	slot.bind<&Owner::read, const Owner>(proxy);
}
#elif CASE == 25
// Competing throwing reference and noexcept value forms must not hide an incompatible callback.
// Public methods:
// - operator(): Expose noexcept mismatch.
struct Mixed {
	template<class T>
	void operator()(T&)
	{}

	void operator()(int) noexcept
	{}
};

void bad()
{
	DelegateSlot<void(int&) noexcept> slot;
	slot.bind(Mixed{});
}

#else
#error "Select a maintained shared case"
#endif
int main()
{}

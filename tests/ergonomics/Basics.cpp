/* Compile-time request bounds, result aliases and exact Field write detection. MIT. */
#include "../structured/traversal/Fixture.hpp"
#include <cassert>
#include <type_traits>
#include <utility>

using namespace fixture;

template<class Field, class Argument>
concept CanWrite =
    requires(const Field& field, Argument&& value) { field.write(std::forward<Argument>(value)); };

using IntegerField = std::remove_cvref_t<decltype(localFields.get<1>())>;
static_assert(CanWrite<IntegerField, std::uint16_t>);
static_assert(CanWrite<IntegerField, const std::uint16_t&>);
static_assert(CanWrite<IntegerField, std::uint16_t&>);
static_assert(!CanWrite<IntegerField, unsigned>);
static_assert(!CanWrite<IntegerField, double>);
static_assert(!CanWrite<IntegerField, volatile std::uint16_t&>);
static_assert(!CanWrite<IntegerField, Config>);
static_assert(std::is_same_v<ts::ServiceResult<Config>::value_type, Config>);
static_assert(std::is_same_v<ts::ServiceResult<void>::value_type, void>);
static_assert(model.maxCommandRequestWireSize() == ts::wireSize<Config>);
static_assert(model.maxServiceRequestWireSize() == ts::wireSize<Config>);
static_assert(model.maxFieldWireSize() == ts::wireSize<Big>);
static_assert(model.maxServiceResponseWireSize() == ts::wireSize<Big>);

// A tree of one empty group bounds nothing.
inline constexpr ts::Model empty{ts::group("empty")};
static_assert(empty.maxCommandRequestWireSize() == 0);
static_assert(empty.maxServiceRequestWireSize() == 0);
inline constexpr ts::Model requestless{ts::group("requestless",
                                                 ts::command<&Device::reset>("Reset", device),
                                                 ts::service<&Device::large>("Large", device))};
static_assert(requestless.maxCommandRequestWireSize() == 0);
static_assert(requestless.maxServiceRequestWireSize() == 0);
static_assert(requestless.maxServiceResponseWireSize() == ts::wireSize<Big>);

ts::CommandResult acceptBig(const Big&) noexcept
{
	++device.commands;
	return ts::CommandResult::Executed;
}

Big echoBig(const Big& value) noexcept
{
	++device.services;
	return value;
}

// Small endpoints, an empty group and large ones: the bounds take the largest.
inline constexpr ts::Model mixed{
    traversalGroup("small"), ts::group("empty"),
    ts::group("large", ts::command<&acceptBig>("Accept"), ts::service<&echoBig>("Echo"))};
static_assert(mixed.maxCommandRequestWireSize() == ts::wireSize<Big>);
static_assert(mixed.maxServiceRequestWireSize() == ts::wireSize<Big>);

int main()
{
	assert(model.maxCommandRequestWireSize() == ts::wireSize<Config>);
	assert(model.maxServiceRequestWireSize() == ts::wireSize<Config>);
	assert(device.reads == 0 && device.writes == 0 && device.commands == 0 && device.services == 0);
	const auto result = ts::ServiceResult<Config>::success(Config{42, true});
	assert(result.hasValue() && result.value().code == 42);
	assert(ts::ServiceResult<void>::success().hasValue());
}

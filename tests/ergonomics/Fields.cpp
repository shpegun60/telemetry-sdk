/*
 * @file Fields.cpp
 * @brief Runtime native Field borrowing and explicit owning read outcomes.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/field/FieldCatalogs.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace ts = telemetry;
using Status = ts::FieldReadStatus;

namespace {
unsigned checks = 0;

void check(bool condition)
{
	++checks;
	if (!condition)
		std::abort();
}

// A native aggregate larger than the convenient service-result bound.
struct Big {
	std::array<std::uint32_t, 256> words;
	friend bool operator==(const Big&, const Big&) = default;
};

using BigAlias = Big;
static_assert(sizeof(Big) >= 1024);
static_assert(sizeof(ts::BorrowedValue<Big>) == sizeof(const Big*));

// The same members do not make a different declaration an exact native type.
struct Other {
	std::array<std::uint32_t, 256> words;
};

enum class Code : std::uint16_t {
	One = 1
};
enum class Position : std::int64_t {
	Borrowed = 0,
	Negative = -1,
	Wide = 0x100000000LL
};

// Application-owned values and call counts; getBig borrows, ownBig owns.
// Public methods: getBig(), ownBig(), getInt(), getReal(), getWide(), getCode().
struct Meter {
	Big big{};
	std::int32_t integer = -3;
	double real = 12.75;
	std::uint64_t wide = std::numeric_limits<std::uint64_t>::max();
	Code code = static_cast<Code>(65535);
	mutable std::array<unsigned, 6> calls{};

	const Big& getBig() const noexcept
	{
		++calls[0];
		return big;
	}

	Big ownBig() const noexcept
	{
		++calls[1];
		return big;
	}

	std::int32_t getInt() const noexcept
	{
		++calls[2];
		return integer;
	}

	double getReal() const noexcept
	{
		++calls[3];
		return real;
	}

	std::uint64_t getWide() const noexcept
	{
		++calls[4];
		return wide;
	}

	Code getCode() const noexcept
	{
		++calls[5];
		return code;
	}
};

Meter functionMeter;

const Big& functionBig() noexcept
{
	return functionMeter.getBig();
}

std::int32_t functionInt() noexcept
{
	return functionMeter.getInt();
}

template<class T>
void expect(const ts::FieldReadResult<T>& result, Status status)
{
	check(result.status() == status);
	check(result.hasValue() == (status == Status::Ok));
	check(static_cast<bool>(result) == result.hasValue());
	check((result.valueOrNull() != nullptr) == result.hasValue());
}

// Result construction can be audited independently of native Field types.
// Public methods: Immobile(), ~Immobile(); copy/move are deliberately absent.
struct Immobile {
	inline static unsigned constructed = 0;
	inline static unsigned destroyed = 0;
	std::array<std::uint8_t, 1024> bytes;

	explicit Immobile(std::uint8_t marker) noexcept : bytes{}
	{
		++constructed;
		bytes[0] = marker;
	}

	Immobile() = delete;
	Immobile(const Immobile&) = delete;
	Immobile(Immobile&&) = delete;
	Immobile& operator=(const Immobile&) = delete;
	Immobile& operator=(Immobile&&) = delete;

	~Immobile() noexcept
	{
		++destroyed;
	}
};

void resultLifetime()
{
	static_assert(!std::is_default_constructible_v<ts::FieldReadResult<Immobile>>);
	static_assert(!std::is_copy_constructible_v<ts::FieldReadResult<Immobile>>);
	static_assert(!std::is_move_constructible_v<ts::FieldReadResult<Immobile>>);
	{
		auto absent = ts::FieldReadResult<Immobile>::failure(Status::NotFound);
		expect(absent, Status::NotFound);
		check(Immobile::constructed == 0 && Immobile::destroyed == 0);
		auto present = ts::FieldReadResult<Immobile>::successFrom([]() -> Immobile {
			return Immobile{91};
		});
		expect(present, Status::Ok);
		check(Immobile::constructed == 1 && present->bytes[0] == 91);
	}
	check(Immobile::constructed == 1 && Immobile::destroyed == 1);

	auto first = ts::FieldReadResult<std::int32_t>::successFrom([]() -> std::int32_t {
		return 41;
	});
	auto failure = ts::FieldReadResult<std::int32_t>::failure(Status::Unavailable);
	auto copied = first;
	check(copied.value() == 41 && *copied == 41);
	failure = copied;
	check(failure.status() == Status::Ok && *failure == 41);
	first = ts::FieldReadResult<std::int32_t>::failure(Status::ConversionFailed);
	expect(first, Status::ConversionFailed);
	copied = first;
	expect(copied, Status::ConversionFailed);
	copied = std::move(failure);
	check(copied.status() == Status::Ok && std::move(copied).value() == 41);
	auto moved = std::move(copied);
	check(moved.status() == Status::Ok && *moved == 41);
	const auto& constant = moved;
	check(constant.valueOrNull() == &constant.value() && *constant == 41);
}

void fields()
{
	Meter meter;
	for (std::size_t i = 0; i < meter.big.words.size(); ++i)
		meter.big.words[i] = static_cast<std::uint32_t>(i * 37u + 11u);
	ts::FunctionSlot<const Big&() noexcept> lateBig;
	ts::FunctionSlot<std::int32_t() noexcept> lateInt;
	ts::OwnerSlot<Meter> owner;
	ts::FieldTable local{ts::field<&Meter::getBig>("borrowed", meter),
	                     ts::field<&Meter::ownBig>("owning", meter),
	                     ts::field<&Meter::getInt>("integer", meter),
	                     ts::field<&Meter::getReal>("real", meter),
	                     ts::field<&Meter::getWide>("wide", meter),
	                     ts::field<&Meter::getCode>("code", meter),
	                     ts::field("late big", lateBig),
	                     ts::field("late integer", lateInt),
	                     ts::field<&Meter::getBig>("late owner", owner)};
	ts::FieldTable empty;
	// The same Fields in a runtime tree after an empty group: meter is group 1.
	ts::Model model{
	    ts::group("empty"),
	    ts::group(
	        "meter", ts::field<&Meter::getBig>("borrowed", meter),
	        ts::field<&Meter::ownBig>("owning", meter), ts::field<&Meter::getInt>("integer", meter),
	        ts::field<&Meter::getReal>("real", meter), ts::field<&Meter::getWide>("wide", meter),
	        ts::field<&Meter::getCode>("code", meter), ts::field("late big", lateBig),
	        ts::field("late integer", lateInt), ts::field<&Meter::getBig>("late owner", owner))};
	const auto& catalogs = model.fields();
	ts::testing::FieldCatalogTable noCatalogs;
	constexpr auto bigId = ts::detail::makeId<1, 0>();
	constexpr auto intId = ts::detail::makeId<1, 2>();
	static_assert(std::same_as<decltype(local.readBorrowed<Big>(0)), ts::BorrowedValue<Big>>);
	static_assert(
	    std::same_as<decltype(catalogs.readBorrowed<BigAlias>(bigId)), ts::BorrowedValue<Big>>);
	static_assert(
	    std::same_as<decltype(local.readAsResult<double>(2)), ts::FieldReadResult<double>>);

	auto view = local.readBorrowed<BigAlias>(Position::Borrowed);
	check(view && view.valueOrNull() == &meter.big && meter.calls[0] == 1);
	const auto copyOfView = view;
	check(copyOfView.valueOrNull() == &meter.big);
	check(catalogs.readBorrowed<Big>(bigId).valueOrNull() == &meter.big && meter.calls[0] == 2);
	check((local.readBorrowed<Big, 0>().valueOrNull() == &meter.big));
	check((ts::testing::CatalogAccess::readBorrowed<BigAlias, bigId>(catalogs).valueOrNull() ==
	       &meter.big));
	const auto beforeMismatch = meter.calls;
	check(!local.readBorrowed<Big>(1));
	check(!local.readBorrowed<Other>(0));
	check(!local.readBorrowed<double>(2));
	check(!catalogs.readBorrowed<Other>(bigId));
	check(meter.calls == beforeMismatch);

	const auto beforeOwn = meter.calls[0];
	auto owned = local.readAsResult<BigAlias>(0);
	expect(owned, Status::Ok);
	check(owned.value() == meter.big && owned.valueOrNull() != &meter.big);
	check(meter.calls[0] == beforeOwn + 1);
	const auto initial = owned->words[0];
	meter.big.words[0] += 1;
	check(view->words[0] == initial + 1 && owned->words[0] == initial);
	const auto beforeOwningGetter = meter.calls[1];
	auto nativeOwned = catalogs.readAsResult<Big>(ts::detail::makeId<1, 1>());
	expect(nativeOwned, Status::Ok);
	check(nativeOwned.value() == meter.big && meter.calls[1] == beforeOwningGetter + 1);
	expect(local.readAsResult<Other>(0), Status::TypeMismatch);
	expect(catalogs.readAsResult<Other>(bigId), Status::TypeMismatch);
	expect(local.readAsResult<double>(0), Status::TypeMismatch);
	check(meter.calls[0] == beforeOwn + 1);

	expect(local.readAsResult<Big>(6), Status::Unavailable);
	expect(local.readAsResult<std::int32_t>(7), Status::Unavailable);
	expect(local.readAsResult<Big>(8), Status::Unavailable);
	// Declared shape is checked before an unavailable binding is observed.
	expect(local.readAsResult<Other>(6), Status::TypeMismatch);
	check(!local.readBorrowed<Big>(6) && !local.readBorrowed<Big>(8));
	expect(catalogs.readAsResult<Big>(ts::detail::makeId<1, 6>()), Status::Unavailable);

	lateBig.bind(&functionBig);
	lateInt.bind(&functionInt);
	owner.bind(meter);
	const auto beforeFunction = functionMeter.calls;
	check(local.readBorrowed<Big>(6).valueOrNull() == &functionMeter.big);
	check(functionMeter.calls[0] == beforeFunction[0] + 1);
	expect(local.readAsResult<Big>(6), Status::Ok);
	check(functionMeter.calls[0] == beforeFunction[0] + 2);
	expect(local.readAsResult<std::int32_t>(7), Status::Ok);
	check(functionMeter.calls[2] == beforeFunction[2] + 1);
	check(local.readBorrowed<Big>(8).valueOrNull() == &meter.big);
	lateBig.reset();
	lateInt.reset();
	owner.reset();
	expect(local.readAsResult<Big>(6), Status::Unavailable);
	expect(local.readAsResult<double>(7), Status::Unavailable);
	check(!local.readBorrowed<Big>(8));

	const auto beforeNumber = meter.calls[2];
	auto numeric = local.readAsResult<double>(2);
	expect(numeric, Status::Ok);
	check(numeric.value() == -3.0 && meter.calls[2] == beforeNumber + 1);
	expect(local.readAsResult<std::uint32_t>(2), Status::ConversionFailed);
	check(meter.calls[2] == beforeNumber + 2);
	check((local.readAsResult<bool, 2>().value()));
	check((ts::testing::CatalogAccess::readAsResult<double, intId>(catalogs).value() == -3.0));
	check(local.get<2>().readAsResult<double>().value() == -3.0);
	check(catalogs.readAsResult<std::int32_t>(intId).value() == -3);
	check(local.readAsResult<std::int32_t>(3).value() == 12);
	expect(local.readAsResult<std::int64_t>(4), Status::ConversionFailed);
	check(local.readAsResult<std::uint16_t>(5).value() == 65535);
	check(local.readAsResult<Code>(2).status() == Status::ConversionFailed);
	meter.integer = 17;
	check(local.readAsResult<Code>(2).value() == static_cast<Code>(17));

	for (const auto nonfinite :
	     {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
	      -std::numeric_limits<double>::infinity()}) {
		meter.real = nonfinite;
		expect(local.readAsResult<std::int32_t>(3), Status::ConversionFailed);
		expect(local.readAsResult<bool>(3), Status::ConversionFailed);
		const auto exact = local.readAsResult<double>(3);
		expect(exact, Status::Ok);
		check(std::isnan(nonfinite) ? std::isnan(*exact) : *exact == nonfinite);
	}
	meter.real = 2147483648.0;
	expect(local.readAsResult<std::int32_t>(3), Status::ConversionFailed);
	meter.real = -2147483648.0;
	check(local.readAsResult<std::int32_t>(3).value() == std::numeric_limits<std::int32_t>::min());
	meter.real = -0.5;
	check(local.readAsResult<std::uint32_t>(3).value() == 0);
	meter.real = std::numeric_limits<double>::max();
	expect(local.readAsResult<float>(3), Status::ConversionFailed);

	const auto beforeInvalid = meter.calls;
	for (const auto bad : {std::int64_t{-1}, std::int64_t{-129}, std::int64_t{9},
	                       std::int64_t{0x100000000LL}, std::numeric_limits<std::int64_t>::max()}) {
		expect(local.readAsResult<Big>(bad), Status::NotFound);
		check(!local.readBorrowed<Big>(bad));
		expect(catalogs.readAsResult<Big>(bad), Status::NotFound);
		check(!catalogs.readBorrowed<Big>(bad));
	}
	expect(local.readAsResult<Big>(std::int8_t{-1}), Status::NotFound);
	expect(local.readAsResult<Big>(Position::Wide), Status::NotFound);
	check(!local.readBorrowed<Big>(Position::Negative));
	expect(local.readAsResult<Big>(std::uint64_t{1} << 32), Status::NotFound);
	expect(catalogs.readAsResult<Big>(std::uint64_t{1} << 32), Status::NotFound);
	expect(catalogs.readAsResult<Big>(ts::detail::makeId<1, 9>()), Status::NotFound);
	expect(catalogs.readAsResult<Big>(ts::detail::makeId<2, 0>()), Status::NotFound);
	expect(catalogs.readAsResult<Big>(ts::detail::makeId<0, 0>()), Status::NotFound);
	expect(empty.readAsResult<Big>(0), Status::NotFound);
	check(!empty.readBorrowed<Big>(0));
	expect(noCatalogs.readAsResult<Big>(0), Status::NotFound);
	check(!noCatalogs.readBorrowed<Big>(0));
	check(meter.calls == beforeInvalid);
}
} // namespace

int main()
{
	resultLifetime();
	fields();
	std::printf("fields runtime checks: %u\n", checks);
}

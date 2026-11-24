/* Multiple used visitor specializations versus named-visitor reuse.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT. */
#include <telemetry/Telemetry.hpp>
#ifdef SCALE_EXECUTE
#include <cstdio>
#endif

namespace ts = telemetry;

namespace scale {
// Native owner supplies repeated Field rows in visitor-specialization size comparisons.
// API: read().
struct Device {
	std::array<std::uint32_t, ROWS> values{};

	template<std::size_t I>
	std::uint32_t read() const noexcept
	{
		return values[I];
	}
};

inline Device device;

constexpr auto names()
{
	std::array<std::array<char, 5>, ROWS> result{};
	for (unsigned i = 0; i < ROWS; ++i)
		result[i] = {'v', char('0' + i / 100), char('0' + i / 10 % 10), char('0' + i % 10), '\0'};
	return result;
}

inline constexpr auto labels = names();

template<std::size_t... I>
constexpr auto make(std::index_sequence<I...>)
{
	return ts::group("scale", ts::field<&Device::read<I>>(labels[I].data(), device)...);
}

// One group of ROWS Fields; its catalog table selects a row by runtime ID.
inline constexpr ts::Model model{make(std::make_index_sequence<ROWS>{})};
inline constexpr auto& fields = model.fields();

// Named visitor reuses one concrete visitor type across all selected Field rows.
// API: operator().
struct ValueVisitor {
	std::uint32_t* result;
	unsigned bias;

	template<class Endpoint>
	void operator()(const Endpoint& endpoint) const noexcept
	{
		*result = endpoint.read().value_or(0) + bias;
	}
};

#define SELECTOR(Name, Bias)                                                                       \
	static std::uint32_t Name(std::uint32_t id) noexcept                                           \
	{                                                                                              \
		std::uint32_t result = 0;                                                                  \
		if constexpr (REUSE) {                                                                     \
			(void)fields.visit(id, ValueVisitor{&result, Bias});                                   \
		} else {                                                                                   \
			const unsigned bias = Bias;                                                            \
			(void)fields.visit(id, [&](const auto& endpoint) {                                     \
				result = endpoint.read().value_or(0) + bias;                                       \
			});                                                                                    \
		}                                                                                          \
		return result;                                                                             \
	}

#if VISITORS > 0
SELECTOR(first, 1)
#endif
#if VISITORS > 1
SELECTOR(second, 2)
#endif
#if VISITORS > 2
SELECTOR(third, 3)
SELECTOR(fourth, 4)
#endif
} // namespace scale

extern "C" std::uint32_t scale_consumer(std::uint32_t id, unsigned operation) noexcept
{
#if VISITORS == 0
	// Baseline does the same native read through the encoded runtime index.
	std::array<std::byte, 4> bytes;
	ts::Workspace workspace{std::span<std::byte>{}};
	const auto status = scale::model.fieldIndex().readEncoded(id, bytes, workspace);
	if (status.dispatch != ts::DispatchStatus::Ok)
		return 0;
	std::uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= std::uint32_t(std::to_integer<unsigned>(bytes[i])) << (8 * i);
	return value + operation + 1;
#else
	switch (operation % VISITORS) {
		case 0:
			return scale::first(id);
#if VISITORS > 1
		case 1:
			return scale::second(id);
#endif
#if VISITORS > 2
		case 2:
			return scale::third(id);
		case 3:
			return scale::fourth(id);
#endif
	}
	return 0;
#endif
}

#ifdef SCALE_EXECUTE
int main()
{
	unsigned checks = 0, failures = 0;
	for (unsigned i = 0; i < ROWS; ++i)
		scale::device.values[i] = i * 3;
	for (unsigned i = 0; i < ROWS; ++i)
		for (unsigned op = 0; op < (VISITORS == 0 ? 1 : VISITORS); ++op) {
			++checks;
			if (scale_consumer(i, op) != i * 3 + op + 1)
				++failures;
		}
	checks += 2;
	if (scale_consumer(ROWS, 0) != 0)
		++failures;
	if (scale_consumer(0x10000u, 0) != 0)
		++failures;
	std::printf("{\"checks\":%u,\"failures\":%u}\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
#endif

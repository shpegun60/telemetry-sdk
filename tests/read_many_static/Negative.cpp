/* Compile refusals must originate at the public static ReadMany API. MIT. */
#include "Contracts.hpp"
#include <integration/read_many/StaticClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <array>

namespace api = static_many_api;
namespace cl = telemetry::client;
namespace rm = telemetry::integration::read_many;

struct ThrowingCall {
	template<class T>
	void operator()(const T&)
	{}
};

struct ThrowingCopy {
	ThrowingCopy() noexcept = default;

	ThrowingCopy(const ThrowingCopy&) noexcept(false)
	{}

	template<class T>
	void operator()(const T&) noexcept
	{}
};

struct CommandLike {
	static constexpr unsigned category = 2;
	using Value = std::uint32_t;
	std::uint32_t id = 0;

	bool readable() const noexcept
	{
		return true;
	}
};

struct WideId {
	static constexpr unsigned category = 1;
	using Value = std::uint32_t;
	std::uint64_t id = UINT64_C(0x100000001);

	bool readable() const noexcept
	{
		return true;
	}
};

struct WrongWidthCodec : static_many_contracts::Codec {
	template<class T>
	static constexpr std::size_t wireSize = 2;
};

struct MissingFingerprintCodec : static_many_contracts::Codec {
private:
	using Codec::fingerprint;
};

template<std::size_t... I>
auto tooMany(std::index_sequence<I...>) noexcept
{
	return rm::fields(((void)I, api::left::F00)...);
}

void probe()
{
	std::array<std::byte, 1024> tx{}, rx{};
	cl::Clock clock{nullptr, +[](void*) noexcept -> std::uint32_t {
		                return 0;
	                }};
	telemetry::integration::encoded::EncodedBackend backend{tx, rx, clock, 30000, true};
#ifdef STATIC_MANY_CALLBACK_SMALL
	api::client::DeviceClient<2, 128, 16> client{backend, clock};
#else
	api::client::DeviceClient<2, 128> client{backend, clock};
#endif
	const auto bound = rm::bindReadMany(client, backend, {17, api::schema::fingerprint},
	                                    rm::fields(api::left::F00));
#if defined(STATIC_MANY_STRUCT)
	(void)rm::fields(api::mixed::Struct);
#elif defined(STATIC_MANY_COMMAND)
	(void)rm::fields(CommandLike{});
#elif defined(STATIC_MANY_EMPTY)
	(void)rm::fields();
#elif defined(STATIC_MANY_33)
	(void)tooMany(std::make_index_sequence<33>{});
#elif defined(STATIC_MANY_THROWING_CALL)
	(void)rm::readMany(client, bound.binding, ThrowingCall{});
#elif defined(STATIC_MANY_THROWING_COPY)
	ThrowingCopy callback;
	(void)rm::readMany(client, bound.binding, callback);
#elif defined(STATIC_MANY_CALLBACK_SMALL)
	(void)rm::readMany(client, bound.binding, [](const auto&) noexcept {
	});
#elif defined(STATIC_MANY_FORGE)
	auto forged = bound.binding;
	forged.ids_[0] = 0;
#elif defined(STATIC_MANY_VALUE_BYPASS)
	rm::StaticReadManyItem<std::uint32_t> item;
	item.value_ = 17;
#elif defined(STATIC_MANY_WRONG_CALLBACK)
	(void)rm::readMany(client, bound.binding, [](const rm::StaticReadManyResult<bool>&) noexcept {
	});
#elif defined(STATIC_MANY_WIDE_ID)
	(void)rm::fields(WideId{});
#elif defined(STATIC_MANY_WRONG_WIDTH)
	cl::DeviceClient<WrongWidthCodec> wrong{backend, clock};
	(void)rm::bindReadMany(wrong, backend, {17, api::schema::fingerprint},
	                       rm::fields(api::mixed::Bool));
#elif defined(STATIC_MANY_NO_FINGERPRINT)
	cl::DeviceClient<MissingFingerprintCodec> wrong{backend, clock};
	(void)rm::bindReadMany(wrong, backend, {17, api::schema::fingerprint},
	                       rm::fields(api::left::F00));
#else
	(void)bound;
#endif
}

/* Public typed Batch compile refusals, never handcrafted invalid wire. MIT. */
#include "Contracts.hpp"
#include <integration/batch/StaticClient.hpp>
#include <integration/encoded/Backend.hpp>
namespace api = static_batch_api;
namespace bt = telemetry::integration::batch;
namespace cl = telemetry::client;

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

struct HugeCodec : api::client::CodecPolicy {
	template<class T>
	static constexpr std::size_t wireSize = SIZE_MAX;
};

template<std::size_t... I>
auto tooMany(std::index_sequence<I...>) noexcept
{
	return bt::items(((void)I, bt::read(api::control::Counter))...);
}

void probe()
{
	std::array<std::byte, 512> tx{}, rx{};
	cl::Clock clock{nullptr, +[](void*) noexcept -> std::uint32_t {
		                return 0;
	                }};
	telemetry::integration::encoded::EncodedBackend backend{tx, rx, clock, 30000, false, true};
#ifdef STATIC_BATCH_CALLBACK_SMALL
	api::client::DeviceClient<2, 256, 16> client{backend, clock};
#else
	api::client::DeviceClient<2, 256> client{backend, clock};
#endif
	api::control::types::Config argument{};
	const auto request = bt::items(bt::write(api::control::Config, argument));
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	const auto bound = bt::bindBatch(client, backend, {17, api::schema::fingerprint}, request);
#if defined(STATIC_BATCH_TEMP_WRITE)
	(void)bt::write(api::control::Config, api::control::types::Config{});
#elif defined(STATIC_BATCH_TEMP_COMMAND)
	(void)bt::command(api::control::Configure, api::control::types::ConfigureRequest{});
#elif defined(STATIC_BATCH_TEMP_SERVICE)
	(void)bt::service(api::control::Echo, api::control::types::EchoRequest{});
#elif defined(STATIC_BATCH_CONVERSION)
	std::uint16_t value = 7;
	(void)bt::write(api::control::Counter, value);
#elif defined(STATIC_BATCH_WRONG_CATEGORY)
	(void)bt::read(api::control::Reset);
#elif defined(STATIC_BATCH_EMPTY)
	(void)bt::items();
#elif defined(STATIC_BATCH_33)
	(void)tooMany(std::make_index_sequence<33>{});
#elif defined(STATIC_BATCH_THROWING_CALL)
	(void)bt::batch(client, bound.binding, request, storage, ThrowingCall{});
#elif defined(STATIC_BATCH_THROWING_COPY)
	ThrowingCopy completion;
	(void)bt::batch(client, bound.binding, request, storage, completion);
#elif defined(STATIC_BATCH_CALLBACK_SMALL)
	(void)bt::batch(client, bound.binding, request, storage, [](const auto&) noexcept {
	});
#elif defined(STATIC_BATCH_HUGE)
	const auto big = bt::items(bt::read(api::control::Counter));
	bt::StaticBatchStorage<HugeCodec, decltype(big)> huge;
	(void)huge;
#elif defined(STATIC_BATCH_PRIVATE_VALUE)
	bt::StaticBatchItemView<api::control::types::Config> view;
	view.value_ = nullptr;
#elif defined(STATIC_BATCH_PRIVATE_BINDING)
	auto changed = bound.binding;
	changed.ids_[0] = 0;
#elif defined(STATIC_BATCH_COPY_STORAGE)
	auto copied = storage;
	(void)copied;
#elif defined(STATIC_BATCH_VOID_VALUE)
	using Item = decltype(bt::write(api::control::Config, argument));
	bt::StaticBatchResult<api::client::CodecPolicy, Item> result;
	(void)result.get<0>()->value();
#else
	(void)storage;
	(void)bound;
#endif
}

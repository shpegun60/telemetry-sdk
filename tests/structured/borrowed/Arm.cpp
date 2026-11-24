/* Real Cortex-M7 native/encoded roots. No execution or device access here. MIT. */
#include "Fixture.hpp"
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <cstdint>

#if defined(BORROWED_FRAME_CONTROL)
extern "C" __attribute__((noinline)) unsigned oversized_frame() noexcept
{
	volatile unsigned char bytes[4096];
	for (unsigned i = 0; i < sizeof(bytes); ++i)
		bytes[i] = static_cast<unsigned char>(i);
	return bytes[4095];
}
#else
using namespace borrowed_fixture;

namespace arm_fixture {
inline Owner<Blob<4096>> kib4{};
inline Owner<Blob<65536>> kib64{};
inline Owner<Blob<4096>> alternateKib4{};

// Fallible borrowed-response owner emitted into offline ARM comparison roots.
// API: call().
template<std::size_t N>
struct WrappedOwner {
	Owner<Blob<N>> owner;
	bool busy = false;

	ts::BorrowedServiceResult<Blob<N>> call() noexcept
	{
		return busy ? ts::BorrowedServiceResult<Blob<N>>::failure(SS::Busy)
		            : ts::BorrowedServiceResult<Blob<N>>::success(owner.get());
	}
};

inline WrappedOwner<4096> wrapped4{};
inline WrappedOwner<65536> wrapped64{};
inline constexpr ts::FieldTable fields{ts::field<&Owner<Blob<4096>>::get>("Kib4", kib4),
                                       ts::field<&Owner<Blob<65536>>::get>("Kib64", kib64)};
inline constexpr ts::ServiceTable services{ts::service<&Owner<Blob<4096>>::get>("Kib4", kib4),
                                           ts::service<&Owner<Blob<65536>>::get>("Kib64", kib64)};
inline constexpr ts::ServiceTable wrappedServices{
    ts::service<&WrappedOwner<4096>::call>("Kib4", wrapped4),
    ts::service<&WrappedOwner<65536>::call>("Kib64", wrapped64)};
// The same endpoints in group trees; the "global" roots reach them by path.
inline constexpr ts::Model model{
    ts::group("fields", ts::field<&Owner<Blob<4096>>::get>("Kib4", kib4),
              ts::field<&Owner<Blob<65536>>::get>("Kib64", kib64)),
    ts::group("services", ts::service<&Owner<Blob<4096>>::get>("Kib4", kib4),
              ts::service<&Owner<Blob<65536>>::get>("Kib64", kib64))};
inline constexpr ts::Model wrappedModel{
    ts::group("services", ts::service<&WrappedOwner<4096>::call>("Kib4", wrapped4),
              ts::service<&WrappedOwner<65536>::call>("Kib64", wrapped64))};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline std::array<std::byte, 256> scratch{};
inline ts::Workspace workspace{scratch};
inline constexpr ::resource::telemetry::v3::ValuesFile values{descriptor, workspace};
inline std::array<std::byte, 65540> output{};
inline volatile unsigned operation = 0;
} // namespace arm_fixture

#define ROOT extern "C" __attribute__((noinline))
#define FIELD_ROOTS(label, N, object, position, path)                                              \
	ROOT ts::BorrowedValue<Blob<N>> field_direct_##label() noexcept                                \
	{                                                                                              \
		return ts::BorrowedValue<Blob<N>>::from(arm_fixture::object.get());                        \
	}                                                                                              \
	ROOT ts::BorrowedValue<Blob<N>> field_local_##label() noexcept                                 \
	{                                                                                              \
		return arm_fixture::fields.read<position>();                                               \
	}                                                                                              \
	ROOT ts::BorrowedValue<Blob<N>> field_global_##label() noexcept                                \
	{                                                                                              \
		return ts::read<arm_fixture::model, path>();                                               \
	}
#if !defined(BORROWED_CODEGEN_CONTROL) && !defined(BORROWED_RELOCATION_CONTROL)
FIELD_ROOTS(kib4, 4096, kib4, 0, "fields/Kib4")
#else
ROOT ts::BorrowedValue<Blob<4096>> field_direct_kib4() noexcept
{
	return ts::BorrowedValue<Blob<4096>>::from(arm_fixture::kib4.get());
}
#ifdef BORROWED_CODEGEN_CONTROL
ROOT ts::BorrowedValue<Blob<4096>> field_local_kib4() noexcept
{
	return {};
}
#else
ROOT ts::BorrowedValue<Blob<4096>> field_local_kib4() noexcept
{
	return ts::BorrowedValue<Blob<4096>>::from(arm_fixture::alternateKib4.get());
}
#endif
ROOT ts::BorrowedValue<Blob<4096>> field_global_kib4() noexcept
{
	return ts::read<arm_fixture::model, "fields/Kib4">();
}
#endif
FIELD_ROOTS(kib64, 65536, kib64, 1, "fields/Kib64")
#define SERVICE_ROOTS(label, N, object, position, path)                                            \
	ROOT ts::BorrowedServiceResult<Blob<N>> service_direct_##label() noexcept                      \
	{                                                                                              \
		return ts::BorrowedServiceResult<Blob<N>>::success(arm_fixture::object.get());             \
	}                                                                                              \
	ROOT ts::BorrowedServiceResult<Blob<N>> service_local_##label() noexcept                       \
	{                                                                                              \
		return arm_fixture::services.call<position>();                                             \
	}                                                                                              \
	ROOT ts::BorrowedServiceResult<Blob<N>> service_global_##label() noexcept                      \
	{                                                                                              \
		return ts::call<arm_fixture::model, path>();                                               \
	}
SERVICE_ROOTS(kib4, 4096, kib4, 0, "services/Kib4")
SERVICE_ROOTS(kib64, 65536, kib64, 1, "services/Kib64")
#define STATUS_ROOTS(label, N, object, position, path)                                             \
	ROOT ts::BorrowedServiceResult<Blob<N>> status_direct_##label() noexcept                       \
	{                                                                                              \
		return arm_fixture::object.call();                                                         \
	}                                                                                              \
	ROOT ts::BorrowedServiceResult<Blob<N>> status_local_##label() noexcept                        \
	{                                                                                              \
		return arm_fixture::wrappedServices.call<position>();                                      \
	}                                                                                              \
	ROOT ts::BorrowedServiceResult<Blob<N>> status_global_##label() noexcept                       \
	{                                                                                              \
		return ts::call<arm_fixture::wrappedModel, path>();                                        \
	}
STATUS_ROOTS(kib4, 4096, wrapped4, 0, "services/Kib4")
STATUS_ROOTS(kib64, 65536, wrapped64, 1, "services/Kib64")

ROOT ts::EncodedReadResult field_encoded(unsigned id, std::span<std::byte> output,
                                         ts::Workspace& workspace) noexcept
{
	return arm_fixture::fields[id].readEncoded(output, workspace);
}

ROOT ts::EncodedCallResult service_encoded(unsigned id, std::span<std::byte> output,
                                           ts::Workspace& workspace) noexcept
{
	return arm_fixture::services[id].callEncoded({}, output, workspace);
}

ROOT ts::EncodedCallResult status_encoded(unsigned id, std::span<std::byte> output,
                                          ts::Workspace& workspace) noexcept
{
	return arm_fixture::wrappedServices[id].callEncoded({}, output, workspace);
}

ROOT ::resource::ReadResult values_encoded(::resource::Cursor cursor,
                                           std::span<std::byte> output) noexcept
{
	return arm_fixture::values.read(cursor, output);
}

extern "C" __attribute__((used, section(".rodata.borrowed_layout")))
const std::uint32_t borrowed_layout[]{sizeof(ts::BorrowedValue<Blob<4096>>),
                                      alignof(ts::BorrowedValue<Blob<4096>>),
                                      sizeof(ts::BorrowedValue<Blob<65536>>),
                                      alignof(ts::BorrowedValue<Blob<65536>>),
                                      sizeof(ts::BorrowedServiceResult<Blob<4096>>),
                                      alignof(ts::BorrowedServiceResult<Blob<4096>>),
                                      sizeof(ts::BorrowedServiceResult<Blob<65536>>),
                                      alignof(ts::BorrowedServiceResult<Blob<65536>>),
                                      sizeof(ts::FieldEntry),
                                      alignof(ts::FieldEntry),
                                      offsetof(ts::FieldEntry, readScratchBytes),
                                      offsetof(ts::FieldEntry, writeScratchBytes)};

ROOT int native_field_roots(unsigned selected) noexcept
{
	switch (selected) {
		case 0:
			return field_direct_kib4()->bytes[0];
		case 1:
			return field_local_kib4()->bytes[0];
		case 2:
			return field_global_kib4()->bytes[0];
		case 3:
			return field_direct_kib64()->bytes[0];
		case 4:
			return field_local_kib64()->bytes[0];
		case 5:
			return field_global_kib64()->bytes[0];
		default:
			return 0;
	}
}

ROOT int native_service_roots(unsigned selected) noexcept
{
	switch (selected) {
		case 6:
			return service_direct_kib4()->bytes[0];
		case 7:
			return service_local_kib4()->bytes[0];
		case 8:
			return service_global_kib4()->bytes[0];
		case 9:
			return service_direct_kib64()->bytes[0];
		case 10:
			return service_local_kib64()->bytes[0];
		case 11:
			return service_global_kib64()->bytes[0];
		default:
			return 0;
	}
}

ROOT int native_status_roots(unsigned selected) noexcept
{
	switch (selected) {
		case 16:
			return status_direct_kib4()->bytes[0];
		case 17:
			return status_local_kib4()->bytes[0];
		case 18:
			return status_global_kib4()->bytes[0];
		case 19:
			return status_direct_kib64()->bytes[0];
		case 20:
			return status_local_kib64()->bytes[0];
		case 21:
			return status_global_kib64()->bytes[0];
		default:
			return 0;
	}
}

ROOT int borrowed_roots() noexcept
{
	using namespace arm_fixture;
	const auto selected = operation;
	switch (selected) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
			return native_field_roots(selected);
		case 6:
		case 7:
		case 8:
		case 9:
		case 10:
		case 11:
			return native_service_roots(selected);
		case 12:
			return static_cast<int>(field_encoded(0u, output, workspace).dispatch);
		case 13:
			return static_cast<int>(field_encoded(1u, output, workspace).dispatch);
		case 14:
			return static_cast<int>(service_encoded(0u, output, workspace).dispatch);
		case 15:
			return static_cast<int>(service_encoded(1u, output, workspace).dispatch);
		case 16:
		case 17:
		case 18:
		case 19:
		case 20:
		case 21:
			return native_status_roots(selected);
		case 22:
			return static_cast<int>(status_encoded(0u, output, workspace).dispatch);
		case 23:
			return static_cast<int>(status_encoded(1u, output, workspace).dispatch);
		default:
			return static_cast<int>(values_encoded(24u, output).status);
	}
}
#endif

/* Compiled Model adapter wrapper scope, separate from Entry/native frames. MIT. */
#include "Fixture.hpp"

using namespace borrowed_fixture;

namespace model_roots_fixture {
inline Owner<Blob<4096>> kib4{};
inline Owner<Blob<65536>> kib64{};
inline constexpr ts::Model model{
    ts::group("fields", ts::field<&Owner<Blob<4096>>::get>("Kib4", kib4),
              ts::field<&Owner<Blob<65536>>::get>("Kib64", kib64)),
    ts::group("services", ts::service<&Owner<Blob<4096>>::get>("Kib4", kib4),
              ts::service<&Owner<Blob<65536>>::get>("Kib64", kib64))};
} // namespace model_roots_fixture

// The adapter accepts ModelView by value. These wrappers intentionally retain
// that boundary, including its view copies, and do not materialize a Response.
extern "C" __attribute__((noinline)) ts::EncodedReadResult
model_field_encoded(unsigned id, std::span<std::byte> output, ts::Workspace& workspace) noexcept
{
	return ts::readFieldEncoded(model_roots_fixture::model.view(), id, output, workspace);
}

extern "C" __attribute__((noinline)) ts::EncodedCallResult
model_service_encoded(unsigned id, std::span<std::byte> output, ts::Workspace& workspace) noexcept
{
	return ts::callServiceEncoded(model_roots_fixture::model.view(), id, {}, output, workspace);
}

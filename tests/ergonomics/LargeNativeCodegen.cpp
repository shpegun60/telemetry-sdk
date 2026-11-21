/*
 * @file LargeNativeCodegen.cpp
 * @brief Placement-result paths detecting hidden 4 KiB native response temporaries.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>
#include <new>

namespace large_native {
namespace ts = telemetry;

struct Request {
	std::uint32_t value;
};

struct Big {
	std::array<std::uint32_t, 1024> values;
};

inline Big backing{};

const Big& read() noexcept
{
	return backing;
}

ts::ServiceResult<Big> service(const Request& request) noexcept
{
	return ts::ServiceResult<Big>::successFrom([&]() -> Big {
		return Big{{request.value}};
	});
}

inline constexpr ts::ServiceTable table{ts::service<&service>("Big")};
inline constexpr ts::testing::ServiceCatalogTable catalogs{ts::testing::group("Buffers", table)};
inline constexpr ts::FieldTable fields{ts::field<&read>("Big")};

struct Visitor {
	void* output;
	const Request& request;

	template<class Definition>
	void operator()(const Definition& definition)
	{
		::new (output) ts::ServiceResult<Big>(definition.call(request));
	}
};
} // namespace large_native

extern "C" void manual_large(void* output, std::uint32_t id, const large_native::Request& request)
{
	large_native::Visitor visitor{output, request};
	(void)large_native::table.visit(id, visitor);
}

extern "C" void convenience_large(void* output, std::uint32_t id,
                                  const large_native::Request& request)
{
	using Result = telemetry::NativeCallResult<telemetry::ServiceResult<large_native::Big>>;
	::new (output) Result(
	    large_native::table.callAs<telemetry::ServiceResult<large_native::Big>>(id, request));
}

extern "C" void convenience_large_global(void* output, std::uint32_t id,
                                         const large_native::Request& request)
{
	using Result = telemetry::NativeCallResult<telemetry::ServiceResult<large_native::Big>>;
	::new (output) Result(
	    large_native::catalogs.callAs<telemetry::ServiceResult<large_native::Big>>(id, request));
}

extern "C" telemetry::BorrowedValue<large_native::Big> borrowed_large(std::uint32_t id)
{
	return large_native::fields.readBorrowed<large_native::Big>(id);
}

/* Direct/get/native-As equivalence and runtime visitor frame probes. MIT. */
#include "Fixture.hpp"
using namespace fixture;

extern "C" std::uint16_t read_direct()
{
	return device.readU16();
}

extern "C" std::uint16_t read_get()
{
	return *localFields.get<1>().read();
}

extern "C" std::uint16_t read_global_get()
{
	return *ts::detail::CatalogAccess::get<1>(fields).read();
}

extern "C" std::uint16_t read_as()
{
	return *localFields.readAs<std::uint16_t, 1>();
}

extern "C" std::uint16_t read_global_as()
{
	return *ts::detail::CatalogAccess::readAs<std::uint16_t, 1>(fields);
}

extern "C" W write_direct(std::uint16_t value)
{
	return device.setU16(value);
}

extern "C" W write_get(std::uint16_t value)
{
	return localFields.get<1>().write(value);
}

extern "C" W write_as(std::uint16_t value)
{
	return localFields.writeAs<1>(value);
}

extern "C" W write_global_as(std::uint16_t value)
{
	return ts::detail::CatalogAccess::writeAs<1>(fields, value);
}

extern "C" C command_direct(const Config& value)
{
	return device.configure(value);
}

extern "C" C command_get(const Config& value)
{
	return localCommands.get<0>().call(value);
}

extern "C" C command_global_get(const Config& value)
{
	return ts::detail::CatalogAccess::get<0>(commands).call(value);
}

extern "C" ts::ServiceResult<Config> service_direct(const Config& value)
{
	return ts::ServiceResult<Config>::successFrom([&]() noexcept {
		return device.echo(value);
	});
}

extern "C" ts::ServiceResult<Config> service_get(const Config& value)
{
	return localServices.get<0>().call(value);
}

extern "C" ts::ServiceResult<Config> service_global_get(const Config& value)
{
	return ts::detail::CatalogAccess::get<0>(services).call(value);
}

unsigned sink;

// Metadata visitor reads erased names to expose its ARM frame without invoking endpoints.
// API: operator().
struct MetadataVisitor {
	template<class Endpoint>
	void operator()(const Endpoint& endpoint) const noexcept
	{
		sink += static_cast<unsigned>(endpoint.name()[0]);
	}
};

extern "C" bool visit_local(std::uint32_t i)
{
	return localFields.visit(i, MetadataVisitor{});
}

extern "C" bool visit_global(std::uint32_t id)
{
	return fields.visit(id, MetadataVisitor{});
}

extern "C" std::optional<double> read_as_runtime(std::uint32_t id)
{
	return fields.readAs<double>(id);
}

extern "C" W write_as_runtime(std::uint32_t id, double value)
{
	return fields.writeAs(id, value);
}

// Both the largest Field Value and Service Response are 4 KiB. A visitor that
// only inspects a definition must still have a small frame, not optional<Big>.
extern "C" bool visit_large_service(std::uint32_t id)
{
	return services.visit(id, MetadataVisitor{});
}

extern "C" const char* get_large_name()
{
	return localFields.get<8>().name();
}

extern "C" std::optional<Big> read_large_as(std::uint32_t id)
{
	return fields.readAs<Big>(id);
}

/*
 * @file main.cpp
 * @brief Device code using the group tree: named handles, path access, and the names it sends.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The application reads and writes its own endpoints through the handles of
 * Telemetry.hpp and through paths, then decodes the descriptor a host would
 * receive and lists every endpoint with its path and type name.
 */

#include "Telemetry.hpp"

#include <telemetry/runtime/RuntimeSchema.hpp>

#include <array>
#include <cstdio>
#include <span>
#include <string_view>

namespace {
namespace ts = telemetry;
namespace rt = telemetry::runtime;

unsigned checks = 0, failures = 0;

void check(bool condition, const char* what)
{
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAILED: %s\n", what);
	}
}

// Packed at compile time: these bytes are what a host reads.
inline constexpr auto packed = ts::descriptor::packDescriptor<device::descriptor>();

void handles()
{
	// A handle reads the whole Field, or one member of it.
	check(device::gain1.read() == 1.0f, "gain1.read()");
	check(device::adcMode.read() == adc::Mode::Continuous, "adcMode.read() reads one member");
	// Writes go through the class's own checks.
	check(device::gain2.write(2.5f) == ts::WriteResult::Applied, "gain2.write(2.5f)");
	check(device::gain2.write(-1.0f) == ts::WriteResult::InvalidValue, "a refused gain");
	check(device::gain2.read() == 2.5f, "the refused gain left the value");
	check(device::adcConfig.write(adc::Config{500, adc::Mode::Triggered, true}) ==
	          ts::WriteResult::Applied,
	      "adcConfig.write(...)");
	check(device::adcMode.read() == adc::Mode::Triggered, "the member follows the write");
	// Commands and Services through their handles, with their arguments.
	check(device::reset.call() == ts::CommandResult::Executed && device::gain2.read() == 1.0f,
	      "reset.call()");
	const auto offset = device::calibrate.call(adc::Reference{1.25f});
	check(offset.hasValue() && offset.value().counts == 2.5f, "calibrate.call(reference)");
	// A handle is an empty object; a copy names the same endpoint.
	const auto meter = device::meterConfig;
	check(meter.write(metrology::Config{20, metrology::Window::FlatTop}) ==
	          ts::WriteResult::Applied,
	      "a stored handle writes");
	static_assert(device::gain1.id == ts::makeId(1, 0) &&
	              device::meterConfig.id == ts::makeId(3, 0));
}

void paths()
{
	// The same access without declaring a handle, and the PackedId of a path.
	check(ts::read<device::model, "metrology/Config.window">() == metrology::Window::FlatTop,
	      "read<model, \"metrology/Config.window\">()");
	check(ts::read<device::model, "metrology/Spectrum.magnitude[1]">() == 1.5f,
	      "an array element of a struct Field");
	check(ts::write<device::model, "adc/channel1/Gain">(4.0f) == ts::WriteResult::Applied &&
	          device::gain1.read() == 4.0f,
	      "write<model, path>(value)");
	check(ts::call<device::model, "adc/Reset">() == ts::CommandResult::Executed,
	      "call<model, path>()");
	static_assert(ts::idOf<device::model, "adc/Calibrate">() == ts::makeId(0, 0));
	static_assert(device::model.idOf<"adc/channel2/Gain">() == ts::makeId(2, 0));
}

// What a host sees: every endpoint's path and the name of its type.
void descriptorNames()
{
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 32> members{};
	std::array<rt::RuntimeEnumEntry, 16> enums{};
	std::array<rt::RuntimeCatalog, 16> catalogs{};
	std::array<rt::RuntimeEndpoint, 16> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(std::span<const std::byte>{packed}, workspace);
	check(bool(parsed), "the descriptor parses");
	if (!parsed)
		return;
	const auto& schema = parsed.schema;
	check(schema.minorVersion() == 1 && schema.groupTree(), "v3.1 with the group tree");
	using Category = ts::descriptor::Category;
	for (const auto category : {Category::Field, Category::Command, Category::Service}) {
		for (const auto& endpoint : schema.endpoints(category)) {
			std::array<char, 64> path{};
			const auto size = schema.endpointPath(endpoint, path);
			const auto name = schema.typeName(endpoint.typeId);
			const auto response = category == Category::Service
			                          ? schema.typeName(endpoint.responseTypeId)
			                          : std::string_view{};
			std::printf("%-8s %-22.*s %.*s%s%.*s\n",
			            category == Category::Field     ? "Field"
			            : category == Category::Command ? "Command"
			                                            : "Service",
			            static_cast<int>(size), path.data(), static_cast<int>(name.size()),
			            name.data(), response.empty() ? "" : " -> ",
			            static_cast<int>(response.size()), response.data());
		}
	}
	const auto named = [&](std::string_view path, std::string_view type) {
		const auto* endpoint = schema.findEndpoint(path);
		return endpoint && schema.typeName(endpoint->typeId) == type;
	};
	check(named("adc/Config", "adc::Config") && named("metrology/Config", "metrology::Config"),
	      "two Configs, two type names");
	check(named("adc/Status", "adc::Status") && named("adc/Calibrate", "adc::Reference"),
	      "automatic names");
	check(named("metrology/Spectrum", "metrology::Spectrum4"), "the TypeReflection name");
	check(schema.findType("adc::Mode") != nullptr, "a member's enum is named too");
}

} // namespace

int main()
{
	handles();
	paths();
	descriptorNames();
	if (failures != 0)
		return 1;
	std::printf("Group tree guide: %u checks passed\n", checks);
	return 0;
}

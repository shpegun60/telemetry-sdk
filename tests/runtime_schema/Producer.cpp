/*
 * @file Producer.cpp
 * @brief Export real Native descriptor, codec and core dispatch bytes.
 * SPDX-License-Identifier: MIT
 */
#include "Fixture.hpp"

#include <cstdio>
#include <cstring>

namespace {
unsigned checks = 0;

// Real producer names deliberately collide only after joining them with '.'.
// Keep categories separate, and include UTF-8 in both a group and an endpoint.
using runtime_fixture::Device;
using runtime_fixture::device;
inline constexpr telemetry::FieldTable lookupFields{
    telemetry::field<&Device::readEmpty>("b.c", device),
    telemetry::field<&Device::readEmpty>("\xCE\xBC", device)};
inline constexpr telemetry::FieldTable lookupLongFields{
    telemetry::field<&Device::readEmpty>("c", device),
    telemetry::field<&Device::readEmpty>("unique", device)};
inline constexpr telemetry::CommandTable lookupCommands{
    telemetry::command<&Device::apply>("b.c", device),
    telemetry::command<&Device::apply>("\xCE\xBC", device)};
inline constexpr telemetry::CommandTable lookupLongCommands{
    telemetry::command<&Device::apply>("c", device),
    telemetry::command<&Device::apply>("unique", device)};
inline constexpr telemetry::ServiceTable lookupServices{
    telemetry::service<&Device::inspect>("b.c", device),
    telemetry::service<&Device::inspect>("\xCE\xBC", device)};
inline constexpr telemetry::ServiceTable lookupLongServices{
    telemetry::service<&Device::inspect>("c", device),
    telemetry::service<&Device::inspect>("unique", device)};
inline constexpr telemetry::FieldCatalogTable lookupFieldCatalogs{
    telemetry::group("a", lookupFields), telemetry::group("a.b", lookupLongFields),
    telemetry::group("\xCE\xB1", lookupLongFields)};
inline constexpr telemetry::CommandCatalogTable lookupCommandCatalogs{
    telemetry::group("a", lookupCommands), telemetry::group("a.b", lookupLongCommands),
    telemetry::group("\xCE\xB1", lookupLongCommands)};
inline constexpr telemetry::ServiceCatalogTable lookupServiceCatalogs{
    telemetry::group("a", lookupServices), telemetry::group("a.b", lookupLongServices),
    telemetry::group("\xCE\xB1", lookupLongServices)};
inline constexpr telemetry::Model lookupModel{lookupFieldCatalogs, lookupCommandCatalogs,
                                              lookupServiceCatalogs};
inline constexpr telemetry::descriptor::Descriptor lookupDescriptor{lookupModel};
inline constexpr auto lookupBytes = telemetry::descriptor::packDescriptor<lookupDescriptor>();

bool check(bool value)
{
	++checks;
	return value;
}

bool save(const char* directory, const char* name, std::span<const std::byte> bytes)
{
	char path[4096];
	if (std::snprintf(path, sizeof(path), "%s/%s", directory, name) >= int(sizeof(path)))
		return false;
	auto* file = std::fopen(path, "wb");
	if (!file)
		return false;
	const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	return std::fclose(file) == 0 && ok;
}
} // namespace

int main(int argc, char** argv)
{
	using namespace runtime_fixture;
	if (argc != 2)
		return 2;
	std::array<std::byte, telemetry::wireSize<Packet>> encoded{}, dispatched{};
	alignas(Packet) std::array<std::byte, sizeof(Packet) + alignof(Packet)> scratch{};
	telemetry::Workspace workspace{scratch};
	if (!check(telemetry::encode(initial, encoded) == telemetry::CodecStatus::Ok))
		return 1;
	const auto result = telemetry::dispatchEncoded(model.view(), telemetry::Operation::FieldRead, 0,
	                                               {}, dispatched, workspace);
	if (!check(result.dispatch == telemetry::DispatchStatus::Ok) ||
	    !check(result.endpointStatus == 0) || !check(result.written == encoded.size()) ||
	    !check(encoded == dispatched) || !check(device.reads == 1))
		return 1;
	Packet decoded{};
	if (!check(telemetry::decode(dispatched, decoded) == telemetry::CodecStatus::Ok) ||
	    !check(decoded.u64 == initial.u64) || !check(decoded.nested.mode == initial.nested.mode) ||
	    !check(std::bit_cast<std::uint32_t>(decoded.f32) == 0x80000000u))
		return 1;
	if (!check(save(argv[1], "descriptor.bin", descriptorBytes)) ||
	    !check(save(argv[1], "payload.bin", encoded)) ||
	    !check(save(argv[1], "lookup-descriptor.bin", lookupBytes)))
		return 1;
	auto variants = initial;
	variants.f32 = std::bit_cast<float>(0x7fc12345u);
	variants.f64 = -std::numeric_limits<double>::infinity();
	variants.flag = false;
	if (!check(telemetry::encode(variants, encoded) == telemetry::CodecStatus::Ok) ||
	    !check(save(argv[1], "variants.bin", encoded)))
		return 1;
	std::printf("runtime native: %u checks, 0 failures\n", checks);
}

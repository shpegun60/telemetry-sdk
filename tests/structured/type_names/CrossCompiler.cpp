/*
 * @file CrossCompiler.cpp
 * @brief One named model built for the device (ARM GCC) and on the host.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The automatic names come from each compiler's own function-signature
 * spelling, so a firmware image and a host build of the same model (emulator,
 * code generator input) must agree byte for byte. run.py compares every
 * compiler's packed bytes with the committed cross.hex: a host compiler runs
 * this program; an ARM compiler only builds the object, and the bytes are
 * taken from its .descriptor.names section. A disagreeing compiler gets the
 * name parser fixed. An automatic name is the innermost scope and the type's
 * own name. The model covers a namespace (device::Mode), nested classes
 * (Board::Probe, Board::Channel), the global scope (Global), the anonymous
 * namespace (Sealed), an inline namespace (v2::Config, named explicitly by
 * rule 4 because Clang spells it device::Config), a class nested in a
 * specialization (unnamed), a typedef-named struct, scoped enums (in a
 * namespace, nested, with an explicit dictionary), an explicit UTF-8 name, a
 * removed name and a specialization. The alias-declared unnamed struct
 * (using X = struct {...}) and the inline namespace's automatic spelling are
 * measured separately in AliasProbe.cpp: compilers disagree there.
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>

#include <array>
#include <cstdint>
#include <cstdio>

namespace device {
namespace ts = telemetry;

enum class Mode : std::uint8_t {
	Idle,
	Run,
	Fault
};

inline namespace v2 {
struct Config {
	float gain;
	bool enabled;
	std::uint32_t periodMs;
};
} // namespace v2

struct Board {
	struct Probe {
		std::int16_t offset;
		Mode mode;
	};
	enum class Channel : std::int8_t {
		Left = -1,
		Right = 1
	};
};

template<class T>
struct Holder {
	struct Inner {
		T value;
	};
};

template<class T>
struct Pair {
	T first;
	T second;
};

typedef struct {
	std::uint8_t raw;
} Legacy;

struct Hidden {
	std::uint8_t flag;
};

enum class Level : std::uint16_t {
	Low = 1,
	High = 400
};

} // namespace device

namespace {
struct Sealed {
	std::uint16_t ticks;
};
} // namespace

struct Global {
	std::int32_t count;
};

// Rule 4: GCC and MSVC spell the inline namespace (v2::Config), Clang drops
// it (device::Config); a model built with Clang names such a type itself.
template<>
struct telemetry::reflection::TypeReflection<device::Config> {
	static constexpr auto name = telemetry::reflection::typeName("v2::Config");
};

template<>
struct telemetry::reflection::TypeReflection<device::Pair<float>> {
	static constexpr auto name = telemetry::reflection::typeName(u8"Пара");
};

template<>
struct telemetry::reflection::TypeReflection<device::Hidden> {
	static constexpr auto name = telemetry::reflection::noTypeName();
};

template<>
struct telemetry::reflection::EnumReflection<device::Level> {
	inline static constexpr auto entries = telemetry::reflection::enumEntries(
	    telemetry::reflection::enumEntry(device::Level::Low, "low"),
	    telemetry::reflection::enumEntry(device::Level::High, "high"));
};

namespace device {
template<class T>
T read() noexcept
{
	return {};
}

// Every shape above, including three structs that stay unnamed (Pair<int>,
// Hidden, and Holder<std::uint8_t>::Inner, whose scope is a specialization).
inline constexpr ts::FieldTable fields{ts::field<&read<Mode>>("mode"),
                                       ts::field<&read<Config>>("config"),
                                       ts::field<&read<Board::Probe>>("probe"),
                                       ts::field<&read<Board::Channel>>("channel"),
                                       ts::field<&read<Holder<std::uint8_t>::Inner>>("inner"),
                                       ts::field<&read<Pair<float>>>("pairF"),
                                       ts::field<&read<Pair<int>>>("pairI"),
                                       ts::field<&read<Legacy>>("legacy"),
                                       ts::field<&read<Hidden>>("hidden"),
                                       ts::field<&read<Level>>("level"),
                                       ts::field<&read<Sealed>>("sealed"),
                                       ts::field<&read<Global>>("global"),
                                       ts::field<&read<std::array<Board::Probe, 2>>>("probes")};
inline constexpr ts::FieldCatalogTable catalogs{ts::group("device", fields)};
inline constexpr ts::ServiceCatalogTable<> noServices{};
inline constexpr ts::Model model{catalogs, ts::emptyCommands, noServices};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = ::telemetry::descriptor::packDescriptor<descriptor>();
} // namespace device

#if defined(CROSS_ARM)
extern "C" {
__attribute__((used, section(".descriptor.names"))) const auto cross_descriptor_names =
    device::bytes;
}
#else
int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	std::FILE* file = std::fopen(argv[1], "wb");
	if (!file)
		return 2;
	const bool ok =
	    std::fwrite(device::bytes.data(), 1, device::bytes.size(), file) == device::bytes.size();
	return std::fclose(file) == 0 && ok ? 0 : 1;
}
#endif

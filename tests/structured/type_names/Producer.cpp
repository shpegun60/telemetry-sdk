/*
 * @file Producer.cpp
 * @brief Native producer side of descriptor v3.1 type names and their switch.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Built twice by run.py: with the default configuration (names on) and with
 * TELEMETRY_TYPE_NAMES=0. It writes the frozen models through the default
 * profile and through withoutTypeNames, plus a model with explicit and removed
 * names, and checks the typeName<T>() seam, the registry's names, streamed
 * reads of a named descriptor and the runtime DuplicateName refusal. The runner
 * compares the written bytes with named.hex and the frozen v3.0 goldens.
 */

#include "../descriptor/Fixture.hpp"

#include <telemetry/abi/StructuredAbi.hpp>

#include <array>
#include <cstdio>
#include <string_view>

namespace names_fixture {
namespace ts = telemetry;

// A specialization has no automatic name; this one gets an explicit UTF-8 name.
template<class T>
struct Pair {
	T first;
	T second;
};

// An automatically named struct whose name is removed.
struct Internal {
	std::uint8_t raw;
};

// A scoped enum renamed explicitly, and an automatically named one.
enum class Phase : std::uint8_t {
	A,
	B
};
enum class Level : std::int16_t {
	Low = -1,
	High = 1
};

// Two short-name clashes, settled only when names are stripped.
namespace left {
struct Status {
	std::uint8_t code;
};
} // namespace left

namespace right {
struct Status {
	std::uint16_t code;
};
} // namespace right

} // namespace names_fixture

template<>
struct telemetry::reflection::TypeReflection<names_fixture::Pair<float>> {
	static constexpr auto name = telemetry::reflection::typeName(u8"PairF²");
};

template<>
struct telemetry::reflection::TypeReflection<names_fixture::Internal> {
	static constexpr auto name = telemetry::reflection::noTypeName();
};

template<>
struct telemetry::reflection::TypeReflection<names_fixture::Phase> {
	static constexpr auto name = telemetry::reflection::typeName("PhaseSelector");
};

namespace names_fixture {

template<class T>
T read() noexcept
{
	return {};
}

inline constexpr ts::FieldTable explicitFields{
    ts::field<&read<Pair<float>>>("Pair"), ts::field<&read<Internal>>("Internal"),
    ts::field<&read<Phase>>("Phase"), ts::field<&read<Level>>("Level"),
    ts::field<&read<std::array<Level, 2>>>("Levels")};
inline constexpr ts::FieldCatalogTable explicitCatalog{ts::group("names", explicitFields)};
inline constexpr ts::ServiceCatalogTable<> noServices{};
inline constexpr ts::Model explicitModel{explicitCatalog, ts::emptyCommands, noServices};
inline constexpr ::telemetry::descriptor::Descriptor explicitDescriptor{explicitModel};
inline constexpr auto explicitBytes = ::telemetry::descriptor::packDescriptor<explicitDescriptor>();

inline constexpr ts::FieldTable clashFields{ts::field<&read<left::Status>>("Left"),
                                            ts::field<&read<right::Status>>("Right")};
inline constexpr ts::FieldCatalogTable clashCatalog{ts::group("clash", clashFields)};
inline constexpr ts::Model clashModel{clashCatalog, ts::emptyCommands, noServices};

// The seam: automatic short names, explicit and removed names, no name on
// arrays, scalars and Void, whatever the switch says about emission.
namespace tr = telemetry::reflection;
static_assert(tr::typeName<fixture::MotorConfig>() == "MotorConfig");
static_assert(tr::typeName<const fixture::Mode&>() == "Mode");
static_assert(ts::Type<fixture::State>::name == "State");
static_assert(tr::typeName<left::Status>() == "Status" &&
              tr::typeName<right::Status>() == "Status");
static_assert(tr::typeName<Pair<float>>() == "PairF\xC2\xB2" && tr::typeName<Pair<int>>().empty());
static_assert(tr::typeName<Internal>().empty() && ts::Type<Internal>::name.empty());
static_assert(tr::typeName<Phase>() == "PhaseSelector" && tr::typeName<Level>() == "Level");
static_assert(tr::typeName<std::array<Level, 2>>().empty() &&
              ts::Type<std::array<Level, 2>>::name.empty());
static_assert(tr::typeName<float>().empty() && tr::typeName<void>().empty());

using ExplicitRegistry = decltype(explicitModel)::Registry;
static_assert(ExplicitRegistry::typeNames().size() == ExplicitRegistry::typeCount);
static_assert(ExplicitRegistry::typeNames()[decltype(explicitModel)::typeId<Phase>()] ==
              "PhaseSelector");

// Every switch value is in the ABI tag, so the two builds name other symbols.
template<std::uint64_t... Parts>
consteval std::uint64_t lastAbiPart(ts::detail::StructuredAbiTag<Parts...>) noexcept
{
	constexpr std::array<std::uint64_t, sizeof...(Parts)> values{Parts...};
	return values.back();
}

static_assert(lastAbiPart(ts::detail::CurrentStructuredAbiTag{}) == TELEMETRY_TYPE_NAMES);

} // namespace names_fixture

namespace {
namespace df = descriptor_fixture;
namespace nf = names_fixture;
namespace ts = telemetry;

int failures = 0;

void check(bool condition, const char* what)
{
	if (!condition) {
		++failures;
		std::printf("FAIL %s\n", what);
	}
}

template<std::size_t N>
bool write(const char* directory, const char* name, const std::array<std::byte, N>& bytes)
{
	char path[1024];
	std::snprintf(path, sizeof path, "%s/%s.bin", directory, name);
	std::FILE* file = std::fopen(path, "wb");
	if (!file)
		return false;
	const bool ok = std::fwrite(bytes.data(), 1, N, file) == N;
	return std::fclose(file) == 0 && ok;
}

// Defaults through the project switch; the profile strips on every build.
inline constexpr ::telemetry::descriptor::Descriptor defaultMixed{fixture::model};
inline constexpr ::telemetry::descriptor::Descriptor defaultEdge{df::model};
inline constexpr ::telemetry::descriptor::Descriptor defaultEmpty{df::emptyModel};
inline constexpr auto strippedMixed = ::telemetry::descriptor::withoutTypeNames(fixture::model);
inline constexpr auto strippedEdge = ::telemetry::descriptor::withoutTypeNames(df::model);
inline constexpr auto strippedEmpty = ::telemetry::descriptor::withoutTypeNames(df::emptyModel);

// A profile that keeps another profile's ceilings and strips names.
struct TightProfile : ts::Limits {
	static constexpr std::uint32_t maxStringBytes = 64;
};

template<class Profile, class F, class C, class S>
constexpr auto withProfile(const ts::Model<F, C, S>& model) noexcept
{
	return ::telemetry::descriptor::Descriptor<F, C, S, Profile>{model};
}

inline constexpr auto tightStripped =
    withProfile<::telemetry::descriptor::WithoutTypeNames<TightProfile>>(fixture::model);

static_assert(strippedMixed.valid() && strippedMixed.fingerprint() == tightStripped.fingerprint());
static_assert(::telemetry::descriptor::packDescriptor<strippedMixed>() == df::mixedBytes);
static_assert(::telemetry::descriptor::packDescriptor<strippedEdge>() == df::edgeBytes);
static_assert(::telemetry::descriptor::packDescriptor<strippedEmpty>() == df::emptyBytes);
// A model without enums or structs is v3.0 even with names on.
static_assert(::telemetry::descriptor::packDescriptor<defaultEmpty>() == df::emptyBytes);
#if TELEMETRY_TYPE_NAMES
static_assert(defaultMixed.size() == strippedMixed.size() + 4 * 3 + 4 + 5 + 11);
static_assert(defaultMixed.fingerprint() != strippedMixed.fingerprint());
#else
static_assert(defaultMixed.fingerprint() == strippedMixed.fingerprint());
static_assert(::telemetry::descriptor::packDescriptor<defaultEdge>() == df::edgeBytes);
#endif

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	const char* directory = argv[1];
	check(
	    write(directory, "default-mixed", ::telemetry::descriptor::packDescriptor<defaultMixed>()),
	    "write default-mixed");
	check(write(directory, "default-edge", ::telemetry::descriptor::packDescriptor<defaultEdge>()),
	      "write default-edge");
	check(
	    write(directory, "default-empty", ::telemetry::descriptor::packDescriptor<defaultEmpty>()),
	    "write default-empty");
	check(write(directory, "strip-mixed", ::telemetry::descriptor::packDescriptor<strippedMixed>()),
	      "write strip-mixed");
	check(write(directory, "strip-edge", ::telemetry::descriptor::packDescriptor<strippedEdge>()),
	      "write strip-edge");
	check(write(directory, "strip-empty", ::telemetry::descriptor::packDescriptor<strippedEmpty>()),
	      "write strip-empty");
	check(write(directory, "explicit", nf::explicitBytes), "write explicit");

	// The streaming form reads the same bytes at every offset, names included.
	const auto packed = ::telemetry::descriptor::packDescriptor<defaultMixed>();
	for (std::size_t offset = 0; offset < packed.size(); ++offset) {
		std::array<std::byte, 7> out{};
		const auto r = defaultMixed.read(offset, out);
		for (std::size_t i = 0; i < r.written; ++i)
			check(out[i] == packed[offset + i], "streamed byte equals packed byte");
	}

	// A runtime-constructed descriptor refuses the clash with DuplicateName
	// when names are emitted; a stripping profile or names off accepts it.
	const ::telemetry::descriptor::Descriptor clash{nf::clashModel};
	const auto stripped = ::telemetry::descriptor::withoutTypeNames(nf::clashModel);
	check(stripped.valid(), "stripped clash descriptor is valid");
#if TELEMETRY_TYPE_NAMES
	check(clash.error() == ::telemetry::descriptor::DescriptorError::DuplicateName &&
	          clash.size() == 0 && clash.fingerprint() == 0,
	      "runtime clash is DuplicateName");
#else
	check(clash.valid() && clash.fingerprint() == stripped.fingerprint(),
	      "names off accepts the clash");
#endif
	std::printf("names %d: producer %s\n", TELEMETRY_TYPE_NAMES, failures == 0 ? "ok" : "failed");
	return failures == 0 ? 0 : 1;
}

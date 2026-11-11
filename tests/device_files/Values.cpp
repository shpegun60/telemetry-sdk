/* values.bin read on a host: ValuesFile writes it as the device serves it, a
 * RuntimeSchema parsed from the descriptor's bytes and readValues read it back,
 * and DynamicValueView reads every value. Then every way a file can be wrong
 * is refused before anything is visited. MIT. */
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <integration/telemetry/v3/ValuesReader.hpp>
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/runtime/DynamicValueView.hpp>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace v3 = resource::telemetry::v3;
namespace rt = telemetry::runtime;

namespace {
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "Values reader check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

enum class Mode : std::uint8_t {
	Idle,
	Run,
	Fault
};

struct Phase {
	float voltage;
	std::int16_t temperature;
};

struct Status {
	Phase phase;
	Mode mode;
	std::array<float, 3> currents;
	bool ok;
};

struct Device {
	std::uint32_t count = 7;
	Status status{{230.5f, -12}, Mode::Run, {1.0f, 2.5f, -3.25f}, true};

	std::uint32_t readCount() noexcept
	{
		return count;
	}

	const Status& readStatus() noexcept
	{
		return status;
	}

	float readGain() noexcept
	{
		return 1.5f;
	}
} device;

// A Field whose owner is not bound: the device serves it Unavailable.
telemetry::OwnerSlot<Device> absent;

constexpr telemetry::FieldTable measure{telemetry::field<&Device::readCount>("Count", device),
                                        telemetry::field<&Device::readStatus>("Status", device)};
constexpr telemetry::FieldTable settings{telemetry::field<&Device::readGain>("Gain", device),
                                         telemetry::field<&Device::readCount>("Spare", absent)};
constexpr telemetry::FieldCatalogTable fields{telemetry::group("measure", measure),
                                              telemetry::group("device", settings)};
constexpr telemetry::CommandCatalogTable commands{};
constexpr telemetry::ServiceCatalogTable services{};
constexpr telemetry::Model model{fields, commands, services};
constexpr telemetry::descriptor::Descriptor descriptor{model};
constexpr auto packed = telemetry::descriptor::packDescriptor<descriptor>();

// Another device: its descriptor has another fingerprint.
constexpr telemetry::FieldTable fewer{telemetry::field<&Device::readCount>("Count", device)};
constexpr telemetry::FieldCatalogTable otherFields{telemetry::group("measure", fewer)};
constexpr telemetry::Model otherModel{otherFields, commands, services};
constexpr telemetry::descriptor::Descriptor otherDescriptor{otherModel};
constexpr auto otherPacked = telemetry::descriptor::packDescriptor<otherDescriptor>();

// A parsed descriptor and the storage it lives in.
struct Schema {
	std::array<rt::RuntimeType, 64> types{};
	std::array<rt::RuntimeMember, 64> members{};
	std::array<rt::RuntimeEnumEntry, 16> enums{};
	std::array<rt::RuntimeCatalog, 8> catalogs{};
	std::array<rt::RuntimeEndpoint, 16> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	rt::RuntimeSchema schema;

	explicit Schema(std::span<const std::byte> bytes)
	{
		const auto parsed = rt::RuntimeSchema::parse(bytes, workspace);
		CHECK(parsed);
		schema = parsed.schema;
	}
};

alignas(32) std::array<std::byte, model.maxScratch() + 1> scratch{};
telemetry::Workspace workspace{scratch};

// The whole file, as a host reads it: in pieces until the end. A token is
// never split, so a piece holds the largest (Status, 21 bytes); 24 still
// takes the file in several reads.
template<class File>
std::vector<std::byte> snapshot(const File& values)
{
	CHECK(values.maxTokenSize() == 21);
	std::vector<std::byte> bytes;
	std::array<std::byte, 24> piece{};
	resource::Cursor cursor = 0;
	for (int guard = 0; guard < 1000; ++guard) {
		const auto result = values.read(cursor, piece);
		CHECK(result.status == resource::Status::Ok);
		bytes.insert(bytes.end(), piece.begin(), piece.begin() + result.written);
		cursor = result.next;
		if (result.eof)
			return bytes;
	}
	CHECK(false);
	return bytes;
}

struct Seen {
	std::string name;
	v3::ValueStatus status;
	std::vector<std::byte> payload;
	std::uint32_t offset;
	telemetry::TypeId type;
};

v3::ValuesProblem read(std::span<const std::byte> bytes, const rt::RuntimeSchema& schema, std::vector<Seen>& seen)
{
	seen.clear();
	return v3::readValues(bytes, schema, [&](const v3::ValuesEntry& entry) {
		seen.push_back({std::string(entry.endpoint->name), entry.status,
		                std::vector<std::byte>(entry.payload.begin(), entry.payload.end()), entry.offset,
		                entry.endpoint->typeId});
	});
}

rt::DynamicValueView view(const rt::RuntimeSchema& schema, const Seen& value)
{
	const auto made = rt::DynamicValueView::create(schema, value.type, value.payload);
	CHECK(made);
	return made.value;
}

template<class T>
T at(const rt::DynamicValueView& root, std::initializer_list<std::string_view> path, std::uint32_t index = ~0u)
{
	auto current = root;
	for (const auto name : path) {
		const auto next = current.member(name);
		CHECK(next);
		current = next.value;
	}
	if (index != ~0u) {
		const auto element = current.index(index);
		CHECK(element);
		current = element.value;
	}
	const auto value = current.template get<T>();
	CHECK(value);
	return value.value;
}

void putU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
	for (std::size_t i = 0; i < 4; ++i)
		bytes[offset + i] = std::byte(value >> (8 * i));
}
} // namespace

int main()
{
	const v3::ValuesFile values{descriptor, workspace};
	const Schema parsed{packed};
	const auto& schema = parsed.schema;
	CHECK(schema.fingerprint() == descriptor.fingerprint());

	// The file as the device serves it, read in pieces.
	const auto bytes = snapshot(values);
	CHECK(bytes.size() == values.size());
	// 24 + (1+4) Count + (1+4+2+1+12+1) Status + (1+4) Gain + (1+4) Spare.
	CHECK(bytes.size() == 24 + 5 + 21 + 5 + 5);

	// The header alone names the descriptor it needs.
	v3::ValuesHeader header;
	CHECK(v3::readValuesHeader(bytes, header) == v3::ValuesProblem::None);
	CHECK(header.major == 3 && header.minor == 0 && header.fieldCount == 4 && header.totalBytes == bytes.size() &&
	      header.fingerprint == descriptor.fingerprint());

	// Every Field in descriptor order, with its status and its wire bytes.
	std::vector<Seen> seen;
	CHECK(read(bytes, schema, seen) == v3::ValuesProblem::None);
	CHECK(seen.size() == 4);
	CHECK(seen[0].name == "Count" && seen[1].name == "Status" && seen[2].name == "Gain" && seen[3].name == "Spare");
	CHECK(seen[0].offset == 24 && seen[1].offset == 29 && seen[2].offset == 50 && seen[3].offset == 55);
	CHECK(seen[0].status == v3::ValueStatus::Ok && seen[1].status == v3::ValueStatus::Ok &&
	      seen[2].status == v3::ValueStatus::Ok && seen[3].status == v3::ValueStatus::Unavailable);
	CHECK(seen[0].payload.size() == 4 && seen[1].payload.size() == 20 && seen[2].payload.size() == 4 &&
	      seen[3].payload.size() == 4);

	// The values themselves, through the schema alone.
	CHECK(view(schema, seen[0]).get<std::uint32_t>().value == 7);
	const auto status = view(schema, seen[1]);
	CHECK(at<float>(status, {"phase", "voltage"}) == 230.5f);
	CHECK(at<std::int16_t>(status, {"phase", "temperature"}) == -12);
	CHECK(at<float>(status, {"currents"}, 0) == 1.0f && at<float>(status, {"currents"}, 2) == -3.25f);
	CHECK(at<bool>(status, {"ok"}));
	const auto mode = status.member("mode");
	CHECK(mode);
	const auto modeName = mode.value.enumName();
	CHECK(modeName && modeName.value.known && modeName.value.name == "Run");
	CHECK(view(schema, seen[2]).get<float>().value == 1.5f);
	for (const auto byte : seen[3].payload)
		CHECK(byte == std::byte{0});

	// Read again after the device changed: the new value, the same layout.
	device.count = 8;
	device.status.mode = Mode::Fault;
	const auto later = snapshot(values);
	CHECK(later.size() == bytes.size());
	CHECK(read(later, schema, seen) == v3::ValuesProblem::None);
	CHECK(view(schema, seen[0]).get<std::uint32_t>().value == 8);
	const auto laterMode = view(schema, seen[1]).member("mode");
	CHECK(laterMode && laterMode.value.enumName().value.name == "Fault");

	// Wrong files: refused with the reason, and nothing is visited.
	const auto refused = [&](std::span<const std::byte> wrong, const rt::RuntimeSchema& against,
	                         v3::ValuesProblem expected) {
		const auto problem = read(wrong, against, seen);
		CHECK(problem == expected);
		CHECK(seen.empty());
		CHECK(!v3::describe(problem).empty() && v3::describe(problem) != v3::describe(v3::ValuesProblem::None));
	};
	refused(std::span(bytes).first(23), schema, v3::ValuesProblem::Short);
	auto wrong = bytes;
	wrong[3] = std::byte{'2'};
	refused(wrong, schema, v3::ValuesProblem::Magic);
	wrong = bytes;
	wrong[4] = std::byte{4};
	refused(wrong, schema, v3::ValuesProblem::Version);
	wrong = bytes;
	wrong[6] = std::byte{1};
	refused(wrong, schema, v3::ValuesProblem::Version);
	// Cut short: the header's size is no longer the file's.
	refused(std::span(bytes).first(bytes.size() - 1), schema, v3::ValuesProblem::Size);
	// Written with another descriptor.
	const Schema other{otherPacked};
	refused(bytes, other.schema, v3::ValuesProblem::Fingerprint);
	wrong = bytes;
	putU32(wrong, 8, 5);
	refused(wrong, schema, v3::ValuesProblem::Count);
	// One byte more than the tokens, the header's size made to agree.
	wrong = bytes;
	wrong.push_back(std::byte{0});
	putU32(wrong, 12, static_cast<std::uint32_t>(wrong.size()));
	refused(wrong, schema, v3::ValuesProblem::Layout);
	// One byte fewer, the size agreeing: the last token ends early.
	wrong = bytes;
	wrong.pop_back();
	putU32(wrong, 12, static_cast<std::uint32_t>(wrong.size()));
	refused(wrong, schema, v3::ValuesProblem::Short);
	// A status the format does not have, on the last Field.
	wrong = bytes;
	wrong[55] = std::byte{2};
	refused(wrong, schema, v3::ValuesProblem::Status);
	// Unavailable with bytes in it: the Spare's, and a Count marked Unavailable.
	wrong = bytes;
	wrong[57] = std::byte{1};
	refused(wrong, schema, v3::ValuesProblem::Padding);
	wrong = bytes;
	wrong[24] = std::byte{1};
	refused(wrong, schema, v3::ValuesProblem::Padding);
	// The same, with its bytes cleared, is a valid file with Count unavailable.
	std::memset(wrong.data() + 25, 0, 4);
	CHECK(read(wrong, schema, seen) == v3::ValuesProblem::None && seen[0].status == v3::ValueStatus::Unavailable);

	std::printf("Values reader: %u checks, 0 failures\n", checks);
	return 0;
}

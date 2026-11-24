/* Measured existing read paths. No new wire operations, scheduler or hardware I/O. MIT. */
#include "DataModel.hpp"
#include "Framing.hpp"
#include <integration/dynamic/DynamicClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <resource/FileSystem.hpp>
#include <resource/protocol/Packets.hpp>
#include <resource/protocol/Protocol.hpp>
#include <telemetry/runtime/DynamicValueView.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <string_view>

namespace rc = read_comparison;
namespace cl = telemetry::client;
namespace rt = telemetry::runtime;
namespace dy = telemetry::integration::dynamic;
namespace en = telemetry::integration::encoded;
namespace rp = resource::protocol;
namespace packets = resource::protocol::client;
namespace tv = resource::telemetry::v3;
using Steady = std::chrono::steady_clock;

namespace {
std::uint64_t checks = 0;

void require(bool condition, std::source_location where = std::source_location::current())
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "Read comparison failed at line %u\n", unsigned(where.line()));
		std::exit(1);
	}
}

std::uint64_t elapsed(Steady::time_point start) noexcept
{
	return std::uint64_t(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(Steady::now() - start).count());
}

template<class U>
U little(resource::Input bytes, std::size_t offset) noexcept
{
	U value = 0;
	for (std::size_t i = 0; i < sizeof(U); ++i)
		value |= U(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
	return value;
}

// Caller-owned indexes outlive the schema and every borrowed value used here.
struct Metadata {
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 4> members{};
	std::array<rt::RuntimeEnumEntry, 1> enums{};
	std::array<rt::RuntimeCatalog, 3> catalogs{}; // one group, three families
	std::array<rt::RuntimeEndpoint, 32> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	rt::RuntimeSchema schema;

	explicit Metadata(rt::Input descriptor) noexcept
	{
		const auto parsed = rt::RuntimeSchema::parse(descriptor, workspace);
		if (!parsed)
			std::fprintf(
			    stderr, "Descriptor parse status=%u error=%u types=%zu members=%zu endpoints=%zu\n",
			    unsigned(parsed.status), unsigned(parsed.error), parsed.requirements.types,
			    parsed.requirements.members, parsed.requirements.endpoints);
		require(bool(parsed));
		schema = parsed.schema;
	}
};

struct Metrics {
	std::uint64_t getter_calls = 0, exchanges = 0, callbacks = 0;
	std::uint64_t protocol_tx_bytes = 0, protocol_rx_bytes = 0, framed_tx_bytes = 0,
	              framed_rx_bytes = 0;
	std::uint64_t returned_scalars = 0;
	std::array<std::uint32_t, 8> owned_values{};
	std::array<bool, 8> received{};

	void traffic(cl::Input request, cl::Input response, bool accountFraming)
	{
		++exchanges;
		protocol_tx_bytes += request.size();
		protocol_rx_bytes += response.size();
		if (accountFraming) {
			const auto tx = rc::framedBytes(request), rx = rc::framedBytes(response);
			require(tx && rx);
			framed_tx_bytes += tx;
			framed_rx_bytes += rx;
		}
	}

	template<class Model>
	void validate() const
	{
		require(std::all_of(received.begin(), received.end(), [](bool x) {
			return x;
		}));
		for (std::size_t i = 0; i < 8; ++i)
			require(owned_values[i] == rc::data[Model::selected[i]]);
	}

	std::uint64_t checksum() const noexcept
	{
		std::uint64_t sum = 0;
		for (auto value : owned_values)
			sum = sum * 33 + value;
		return sum;
	}
};

struct Timing {
	std::uint64_t iterations = 0, host_elapsed_ns = 0, dispatcher_elapsed_ns = 0, checksum = 0;
};

struct Mode {
	const char* name = nullptr;
	Metrics metrics{}, warm_setup{};
	bool warm = false;
	std::size_t resource_payload_capacity = 0;
	Timing timing{};
};

struct ProfileReport {
	const char* name;
	std::size_t field_count = 0, client_context_sizeof = 0, metadata_sizeof = sizeof(Metadata),
	            values_provider_sizeof = 0;
	std::size_t owner_sizeof = 0, workspace_capacity_bytes = 0, protocol_buffer_bytes = 0;
	std::uint64_t scalar_fingerprint = 0, aggregate_fingerprint = 0;
	std::array<std::uint32_t, 8> ids{}, expected{};
	std::array<Mode, 6> modes{};
};

// One admitted runtime client per alternative Model. Construction prepares
// bindings; cycle() drains its existing FIFO before its local Metrics expires.
template<class Model>
class EncodedLoop {
public:
	Metadata metadata;
	std::uint32_t now = 0;
	cl::Clock clock{&now, +[](void* p) noexcept {
		                return *static_cast<std::uint32_t*>(p);
	                }};
	std::array<std::byte, 32> tx{}, port{};
	std::array<std::byte, 2048> result{}, reply{};
	std::array<std::byte, 0> scratch{};
	telemetry::Workspace workspace{scratch};
	en::EncodedBackend backend{tx, result, clock};
	dy::DynamicClient<8, 64, 192> client{backend, clock};
	telemetry::ModelView model;
	std::array<dy::EndpointBinding, 8> bindings{};
	bool aggregate;

	explicit EncodedLoop(bool aggregated) noexcept
	    : metadata(aggregated ? rt::Input{Model::aggregateBytes} : rt::Input{Model::scalarBytes}),
	      model(aggregated ? Model::aggregateModel.view() : Model::scalarModel.view()),
	      aggregate(aggregated)
	{
		require(client.admit(metadata.schema, metadata.schema.descriptor().first(64), 1) ==
		        cl::Status::Ok);
		const auto count = aggregate ? 1U : 8U;
		for (std::size_t i = 0; i < count; ++i) {
			const auto bound =
			    client.bind(cl::Operation::FieldRead, aggregate ? 0 : Model::selected[i]);
			require(bool(bound));
			bindings[i] = bound.endpoint;
		}
	}

	~EncodedLoop()
	{
		client.close();
		for (unsigned i = 0; i < 32 && !client.drained(); ++i)
			client.process(now);
		require(client.drained());
	}

	Metrics cycle(bool accountFraming, bool measureDispatcher, std::uint64_t& dispatchTime)
	{
		Metrics metrics;
		const auto count = aggregate ? 1U : 8U;
		for (std::size_t i = 0; i < count; ++i) {
			const auto submitted =
			    client.readField(bindings[i], [&, i](const dy::DynamicResult& value) noexcept {
				    ++metrics.callbacks;
				    require(value.operation.succeeded() && value.hasValue());
				    if (aggregate) {
					    const auto values = value.value.member("values");
					    require(bool(values));
					    for (std::uint32_t j = 0; j < 8; ++j) {
						    const auto child = values.value.index(j);
						    require(bool(child));
						    const auto number = child.value.template get<std::uint32_t>();
						    require(bool(number));
						    metrics.owned_values[j] = number.value;
						    metrics.received[j] = true;
						    ++metrics.returned_scalars;
					    }
				    } else {
					    const auto number = value.value.template get<std::uint32_t>();
					    require(bool(number));
					    metrics.owned_values[i] = number.value;
					    metrics.received[i] = true;
					    ++metrics.returned_scalars;
				    }
			    });
			require(bool(submitted));
		}
		for (unsigned step = 0; step < 256 && client.pending(); ++step) {
			client.process(now);
			const auto packet = client.nextPacket();
			if (!packet)
				continue;
			require(packet.bytes.size() <= port.size());
			std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
			require(client.prepareHandoff(packet.session, packet.requestId));
			const auto request = cl::Input{port}.first(packet.bytes.size());
			const auto start = measureDispatcher ? Steady::now() : Steady::time_point{};
			const auto dispatched =
			    en::dispatchEncoded(model, workspace, packet.session, request, reply);
			if (measureDispatcher)
				dispatchTime += elapsed(start);
			require(bool(dispatched));
			const auto response = cl::Input{reply}.first(dispatched.written);
			metrics.traffic(request, response, accountFraming);
			require(client.onSendAccepted(packet.session, packet.requestId));
			require(client.onReply(response, packet.session, packet.requestId));
			// Borrowed callback views must never be needed after reply handling.
			// This deliberate canary overwrite belongs only to correctness;
			// it is not normal per-exchange work in the timed owner loop.
			if (accountFraming) {
				std::fill(reply.begin(), reply.end(), std::byte{0xa5});
				std::fill(result.begin(), result.end(), std::byte{0x5a});
			}
		}
		require(client.pending() == 0);
		return metrics;
	}
};

// Benchmark-only sequential Resource exchanges, not a new client scheduler.
// Each borrowed chunk is consumed before reply storage is reused.
template<class Model>
class ResourceLoop {
public:
	Metadata metadata{Model::scalarBytes};
	std::array<std::byte, 0> scratch{};
	telemetry::Workspace workspace{scratch};
	using Values =
	    decltype(tv::ValuesFile{Model::scalarDescriptor, std::declval<telemetry::Workspace&>()});
	Values values{Model::scalarDescriptor, workspace};
	const resource::FileSystem<1> files{
	    resource::filesystem(resource::file("/telemetry/values.bin", values))};
	std::array<std::byte, rp::wire::readRequestSize> request{};
	std::array<std::byte, 2048> reply{};
	Metrics headerSetup{};

	ResourceLoop()
	{
		require(values.size() == 24 + 5 * Model::fieldCount && values.maxTokenSize() == 5 &&
		        values.requiredWorkspace() == 0);
		// Header/fingerprint admission is real Resource traffic, excluded only
		// from warm-cycle metrics and explicitly reported as setup below.
		std::uint64_t unused = 0;
		const auto header = read(0, 24, headerSetup, true, false, unused);
		checkHeader(header.data);
		require(header.next == 24 && !header.eof);
		require(Model::owner.calls() == 0);
	}

	void checkHeader(resource::Input bytes) const
	{
		require(bytes.size() >= 24 && bytes[0] == std::byte{'T'} && bytes[1] == std::byte{'V'} &&
		        bytes[2] == std::byte{'L'} && bytes[3] == std::byte{'3'});
		require(little<std::uint16_t>(bytes, 4) == 3 && little<std::uint16_t>(bytes, 6) == 0);
		require(little<std::uint32_t>(bytes, 8) == Model::fieldCount &&
		        little<std::uint32_t>(bytes, 12) == values.size());
		require(little<std::uint64_t>(bytes, 16) == metadata.schema.fingerprint());
	}

	packets::ReadReply read(resource::Cursor cursor, std::size_t capacity, Metrics& metrics,
	                        bool framing, bool measure, std::uint64_t& dispatchTime)
	{
		require(capacity <= reply.size() - rp::wire::chunkReplyHeaderSize);
		const auto built = packets::makeRead(request, 0, cursor);
		require(built.status == packets::BuildStatus::Ok);
		const auto input = cl::Input{request}.first(built.written);
		const auto start = measure ? Steady::now() : Steady::time_point{};
		const auto dispatched = rp::process(
		    files.view(), input, std::span{reply}.first(rp::wire::chunkReplyHeaderSize + capacity));
		if (measure)
			dispatchTime += elapsed(start);
		require(dispatched.status == resource::Status::Ok);
		const auto output = cl::Input{reply}.first(dispatched.written);
		metrics.traffic(input, output, framing);
		const auto parsed = packets::parseRead(output);
		require(bool(parsed) && parsed.response.status == resource::Status::Ok);
		require(parsed.response.next == cursor + parsed.response.data.size());
		return parsed.response;
	}

	Metrics cycle(bool warm, std::size_t capacity, bool framing, bool measure,
	              std::uint64_t& dispatchTime)
	{
		Metrics metrics;
		resource::Cursor cursor = warm ? 24 : 0;
		std::size_t position = 0;
		bool ended = false;
		const auto endpoints = metadata.schema.endpoints(telemetry::descriptor::Category::Field);
		require(endpoints.size() == Model::fieldCount);
		for (std::size_t page = 0; page < Model::fieldCount + 2; ++page) {
			const auto chunk = read(cursor, capacity, metrics, framing, measure, dispatchTime);
			require(chunk.next > cursor); // No retry loop may mask a capacity mismatch.
			auto bytes = chunk.data;
			if (cursor == 0) {
				checkHeader(bytes);
				bytes = bytes.subspan(24);
			}
			require(bytes.size() % 5 == 0);
			while (!bytes.empty()) {
				require(position < endpoints.size() && bytes[0] == std::byte{0});
				const auto value =
				    metadata.schema.view(endpoints[position].typeId, bytes.subspan(1, 4));
				require(bool(value));
				const auto number = value.value.template get<std::uint32_t>();
				require(bool(number));
				++metrics.returned_scalars;
				for (std::size_t i = 0; i < 8; ++i)
					if (Model::selected[i] == endpoints[position].id) {
						metrics.owned_values[i] = number.value;
						metrics.received[i] = true;
					}
				++position;
				bytes = bytes.subspan(5);
			}
			cursor = chunk.next;
			if (framing)
				std::fill(reply.begin(), reply.end(), std::byte{0xa5});
			if (chunk.eof) {
				ended = true;
				break;
			}
		}
		require(ended && cursor == values.size() && position == Model::fieldCount);
		return metrics;
	}

	void insufficientCapacity()
	{
		const auto before = Model::owner.calls();
		const auto built = packets::makeRead(request, 0, 24);
		require(built.status == packets::BuildStatus::Ok);
		const auto response = rp::process(files.view(), cl::Input{request}.first(built.written),
		                                  std::span{reply}.first(12 + 4));
		const auto parsed = packets::parseRead(cl::Input{reply}.first(response.written));
		require(bool(parsed) && parsed.response.status == resource::Status::BufferTooSmall &&
		        parsed.response.next == 24 && parsed.response.data.empty() && !parsed.response.eof);
		require(Model::owner.calls() == before);
	}
};

void framingControls()
{
	using Link = cobs::Endpoint<wire::Pool<1, 1>, telemetry::integration::serial::FrameFormat>;
	std::array<std::byte, 2049> body{};
	for (std::size_t i = 0; i < body.size(); ++i)
		body[i] = std::byte((i * 37u) & 0xffu);
	for (const auto size : {0U, 1U, 253U, 254U, 255U, 2048U}) {
		Link sender, receiver;
		std::size_t observed = 0;
		require(sender.bind(
		    [&](std::span<const std::uint8_t> frame) noexcept {
			    observed = frame.size();
			    receiver.consume(frame);
			    return true;
		    },
		    []() noexcept {
			    return false;
		    }));
		auto message = sender.make_message(size);
		require(bool(message));
		require(message.append_bytes({reinterpret_cast<const std::uint8_t*>(body.data()), size}));
		require(sender.send(message) == wire::SendResult::Sent);
		sender.poll(0);
		require(!sender.tx_active());
		auto packet = receiver.pop_packet();
		require(bool(packet) && packet.size() == size);
		require(std::equal(packet.data().begin(), packet.data().end(),
		                   reinterpret_cast<const std::uint8_t*>(body.data())));
		require(rc::framedBytes(cl::Input{body}.first(size)) == observed && observed > 0);
		packet.reset();
	}
	require(rc::framedBytes(body) == 0);
}

template<class Model>
ProfileReport profile(const char* name, std::uint64_t iterations)
{
	ProfileReport report{name};
	report.field_count = Model::fieldCount;
	report.ids = Model::selected;
	for (std::size_t i = 0; i < 8; ++i)
		report.expected[i] = rc::data[Model::selected[i]];
	report.scalar_fingerprint = Model::scalarDescriptor.fingerprint();
	report.aggregate_fingerprint = Model::aggregateDescriptor.fingerprint();
	require(report.scalar_fingerprint != report.aggregate_fingerprint);
	report.client_context_sizeof = sizeof(EncodedLoop<Model>);
	report.owner_sizeof = sizeof(typename Model::Owner);
	report.protocol_buffer_bytes = 32 + 32 + 2048 + 2048;
	report.values_provider_sizeof =
	    sizeof(tv::ValuesFile{Model::scalarDescriptor, std::declval<telemetry::Workspace&>()});
	for (unsigned kind = 0; kind < 2; ++kind) {
		auto& mode = report.modes[kind];
		mode.name = kind ? "aggregate" : "individual";
		EncodedLoop<Model> loop{kind != 0};
		Model::owner.reset();
		std::uint64_t unused = 0;
		mode.metrics = loop.cycle(true, false, unused);
		mode.metrics.getter_calls = Model::owner.calls();
		mode.metrics.template validate<Model>();
		require(mode.metrics.getter_calls == (kind ? 1 : 8) &&
		        mode.metrics.exchanges == (kind ? 1 : 8) &&
		        mode.metrics.callbacks == (kind ? 1 : 8));
		for (std::uint64_t i = 0; i < iterations; ++i) {
			Model::owner.reset();
			const auto start = Steady::now();
			const auto result = loop.cycle(false, true, mode.timing.dispatcher_elapsed_ns);
			mode.timing.host_elapsed_ns += elapsed(start);
			mode.timing.checksum += result.checksum();
			result.template validate<Model>();
			require(Model::owner.calls() == mode.metrics.getter_calls);
		}
		mode.timing.iterations = iterations;
	}
	Model::owner.reset();
	ResourceLoop<Model> loop;
	loop.insufficientCapacity();
	for (unsigned kind = 0; kind < 4; ++kind) {
		auto& mode = report.modes[2 + kind];
		mode.name = kind == 0   ? "values_cold"
		            : kind == 1 ? "values_warm"
		            : kind == 2 ? "values_paged_cold"
		                        : "values_paged_warm";
		mode.warm = (kind % 2) == 1;
		mode.resource_payload_capacity = kind < 2 ? 2036 : 32;
		mode.warm_setup = loop.headerSetup;
		Model::owner.reset();
		std::uint64_t unused = 0;
		mode.metrics = loop.cycle(mode.warm, mode.resource_payload_capacity, true, false, unused);
		mode.metrics.getter_calls = Model::owner.calls();
		mode.metrics.template validate<Model>();
		const auto expectedPages = kind < 2    ? 1
		                           : mode.warm ? (Model::fieldCount + 5) / 6
		                                       : 1 + (Model::fieldCount - 1 + 5) / 6;
		require(mode.metrics.getter_calls == Model::fieldCount &&
		        mode.metrics.returned_scalars == Model::fieldCount &&
		        mode.metrics.exchanges == expectedPages);
		require(mode.metrics.protocol_tx_bytes == 13 * expectedPages &&
		        mode.metrics.protocol_rx_bytes ==
		            12 * expectedPages + 5 * Model::fieldCount + (mode.warm ? 0 : 24));
		for (std::uint64_t i = 0; i < iterations; ++i) {
			Model::owner.reset();
			const auto start = Steady::now();
			const auto result = loop.cycle(mode.warm, mode.resource_payload_capacity, false, true,
			                               mode.timing.dispatcher_elapsed_ns);
			mode.timing.host_elapsed_ns += elapsed(start);
			mode.timing.checksum += result.checksum();
			result.template validate<Model>();
			require(Model::owner.calls() == mode.metrics.getter_calls);
		}
		mode.timing.iterations = iterations;
	}
	return report;
}

void numbers(std::FILE* out, const std::array<std::uint32_t, 8>& values)
{
	std::fputc('[', out);
	for (std::size_t i = 0; i < values.size(); ++i)
		std::fprintf(out, "%s%" PRIu32, i ? "," : "", values[i]);
	std::fputc(']', out);
}

void metrics(std::FILE* out, const Metrics& m, bool selected = true)
{
	std::fprintf(out,
	             "\"getter_calls\":%" PRIu64 ",\"exchanges\":%" PRIu64 ",\"callbacks\":%" PRIu64
	             ",\"protocol_tx_bytes\":%" PRIu64 ",\"protocol_rx_bytes\":%" PRIu64
	             ",\"framed_tx_bytes\":%" PRIu64 ",\"framed_rx_bytes\":%" PRIu64,
	             m.getter_calls, m.exchanges, m.callbacks, m.protocol_tx_bytes, m.protocol_rx_bytes,
	             m.framed_tx_bytes, m.framed_rx_bytes);
	if (selected) {
		std::fprintf(out,
		             ",\"returned_scalars\":%" PRIu64
		             ",\"selected_scalars\":8,\"correct\":true,\"checksum\":%" PRIu64
		             ",\"owned_values\":",
		             m.returned_scalars, m.checksum());
		numbers(out, m.owned_values);
	}
}

void json(std::FILE* out, const std::array<ProfileReport, 2>& reports, std::uint64_t iterations)
{
	std::fprintf(
	    out,
	    "{\"passed\":true,\"hardware\":false,\"wire_batch_implemented\":false,\"coherent_snapshot\":false,\"descriptor_setup_excluded\":true,\"host_timing_excludes_framing\":true,\"checks\":%" PRIu64
	    ",\"timing_iterations\":%" PRIu64
	    ",\"framing_controls\":{\"empty\":true,\"max\":true,\"oversize\":true,\"roundtrip\":true,\"cases\":6},\"timing_note\":\"Instrumented owner loop includes counters, runtime validation and timer overhead; excludes setup, admission, bind, framing and poison\",\"uart_airtime_note\":\"Calculated bidirectional 8N1 airtime from the accounting cycle's actual framed bytes; excludes setup, processing and jitter; not hardware timing\",\"memory_note\":\"sizeof and explicit capacities for this ABI; not peak RSS, MCU stack or linked Flash\",\"profiles\":[",
	    checks, iterations);
	for (std::size_t p = 0; p < reports.size(); ++p) {
		const auto& r = reports[p];
		std::fprintf(out, "%s{\"name\":\"%s\",\"field_count\":%zu,\"requested_ids\":", p ? "," : "",
		             r.name, r.field_count);
		numbers(out, r.ids);
		std::fputs(",\"expected_values\":", out);
		numbers(out, r.expected);
		std::fprintf(
		    out,
		    ",\"scalar_descriptor_fingerprint\":\"%016" PRIx64
		    "\",\"aggregate_descriptor_fingerprint\":\"%016" PRIx64
		    "\",\"different_descriptor_projection\":true,\"memory\":{\"client_context_sizeof\":%zu,\"metadata_sizeof\":%zu,\"values_provider_sizeof\":%zu,\"owner_sizeof\":%zu,\"workspace_capacity_bytes\":%zu,\"protocol_buffer_bytes\":%zu,\"owned_array_bytes\":%zu},\"modes\":[",
		    r.scalar_fingerprint, r.aggregate_fingerprint, r.client_context_sizeof,
		    r.metadata_sizeof, r.values_provider_sizeof, r.owner_sizeof, r.workspace_capacity_bytes,
		    r.protocol_buffer_bytes, sizeof(std::array<std::uint32_t, 8>));
		for (std::size_t i = 0; i < r.modes.size(); ++i) {
			const auto& m = r.modes[i];
			std::fprintf(out, "%s{\"name\":\"%s\",", i ? "," : "", m.name);
			metrics(out, m.metrics);
			std::fprintf(out,
			             ",\"resource_payload_capacity\":%zu,\"warm_cursor\":%u,\"warm_setup\":",
			             m.resource_payload_capacity, m.warm ? 24 : 0);
			if (m.warm) {
				std::fputc('{', out);
				metrics(out, m.warm_setup, false);
				std::fputc('}', out);
			} else
				std::fputs("null", out);
			const auto framed = m.metrics.framed_tx_bytes + m.metrics.framed_rx_bytes;
			const auto airtime = [framed](std::uint64_t baud) noexcept {
				return (framed * UINT64_C(10000000000) + baud - 1) / baud;
			};
			std::fprintf(out,
			             ",\"uart_airtime_ns\":{\"1200\":%" PRIu64 ",\"115200\":%" PRIu64
			             ",\"4000000\":%" PRIu64 "}",
			             airtime(1200), airtime(115200), airtime(4000000));
			std::fprintf(
			    out,
			    ",\"timing\":{\"clock\":\"std::chrono::steady_clock\",\"iterations\":%" PRIu64
			    ",\"host_elapsed_ns\":%" PRIu64 ",\"dispatcher_elapsed_ns\":%" PRIu64
			    ",\"framing_included\":false,\"poison_included\":false,\"checksum\":%" PRIu64 "}}",
			    m.timing.iterations, m.timing.host_elapsed_ns, m.timing.dispatcher_elapsed_ns,
			    m.timing.checksum);
		}
		std::fputs("]}", out);
	}
	std::fputs("]}\n", out);
}
} // namespace

int main(int argc, char** argv)
{
	const char* report = nullptr;
	std::uint64_t iterations = 200;
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "--report" && i + 1 < argc && !report)
			report = argv[++i];
		else if (arg == "--iterations" && i + 1 < argc) {
			const std::string_view value = argv[++i];
			const auto parsed =
			    std::from_chars(value.data(), value.data() + value.size(), iterations);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
			    iterations == 0 || iterations > 100000)
				return 2;
		} else {
			std::fprintf(stderr,
			             "Usage: read-comparison [--report file.json] [--iterations 1..100000]\n");
			return 2;
		}
	}
	framingControls();
	const std::array reports{profile<rc::All8>("all8", iterations),
	                         profile<rc::Sparse8of32>("sparse8of32", iterations)};
	if (report) {
		auto* out = std::fopen(report, "wb");
		if (!out) {
			std::perror("report");
			return 3;
		}
		json(out, reports, iterations);
		const bool ok = std::ferror(out) == 0;
		const auto closed = std::fclose(out);
		if (!ok || closed != 0)
			return 3;
	}
	for (const auto& p : reports)
		for (const auto& m : p.modes)
			std::printf("%s/%s getters=%" PRIu64 " exchanges=%" PRIu64 " protocol=%" PRIu64
			            " framed=%" PRIu64 " host_ns=%" PRIu64 " dispatcher_ns=%" PRIu64 "\n",
			            p.name, m.name, m.metrics.getter_calls, m.metrics.exchanges,
			            m.metrics.protocol_tx_bytes + m.metrics.protocol_rx_bytes,
			            m.metrics.framed_tx_bytes + m.metrics.framed_rx_bytes,
			            m.timing.host_elapsed_ns / iterations,
			            m.timing.dispatcher_elapsed_ns / iterations);
	std::printf("Read comparison checks=%" PRIu64 " profiles=2 modes=12\n", checks);
	return 0;
}

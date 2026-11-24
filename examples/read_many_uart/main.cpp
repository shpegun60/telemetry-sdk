/*
 * @file main.cpp
 * @brief Explicit ReadMany qualification over the existing serial transport and one FIFO.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <integration/qt/SerialTransport.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/read_many/Client.hpp>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace cl = telemetry::client;
namespace rt = telemetry::runtime;
namespace dy = telemetry::integration::dynamic;
namespace rm = telemetry::integration::read_many;
namespace wire = telemetry::protocol::encoded::read_many;

namespace {
cl::Input input(const QByteArray& bytes)
{
	return {reinterpret_cast<const std::byte*>(bytes.constData()), std::size_t(bytes.size())};
}

QByteArray copy(cl::Input bytes)
{
	return {reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())};
}

QString sha256(const QByteArray& bytes)
{
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}

struct Completion {
	cl::Status status = cl::Status::Busy;
	bool success = false, valid = false;
	unsigned callbacks = 0;
	QByteArray bytes;
};

constexpr std::array<unsigned, 4> loadCounts{8, 12, 16, 32};

struct LoadMetrics {
	quint64 attempted = 0, completed = 0, callbacks = 0;
	quint64 requestBytes = 0, replyBytes = 0, ok = 0, unavailable = 0;
	quint64 minimumUs = 0, maximumUs = 0, sumUs = 0;

	void accepted(unsigned count) noexcept
	{
		++attempted;
		requestBytes += 2 + 4 * count;
	}

	void validated(quint64 microseconds, unsigned count, unsigned unavailableCount) noexcept
	{
		if (!completed || microseconds < minimumUs)
			minimumUs = microseconds;
		maximumUs = std::max(maximumUs, microseconds);
		sumUs += microseconds;
		++completed;
		ok += count - unavailableCount;
		unavailable += unavailableCount;
	}

	QJsonObject json() const
	{
		return {{"attempted", double(attempted)},
		        {"completed", double(completed)},
		        {"callbacks", double(callbacks)},
		        {"requestPayloadBytes", double(requestBytes)},
		        {"replyPayloadBytes", double(replyBytes)},
		        {"okItems", double(ok)},
		        {"unavailableItems", double(unavailable)},
		        {"latencyUs", QJsonObject{{"min", double(minimumUs)},
		                                  {"max", double(maximumUs)},
		                                  {"sum", double(sumUs)}}}};
	}
};

// Application storage, not another request scheduler. The portable client owns
// its sole FIFO; Qt owns physical frame copies and all diagnostic JSON strings.
class Check {
	// Pending callbacks always target this owned slot, including exception drain.
	Completion completion_;
	QElapsedTimer elapsed_;
	std::uint32_t advance_ = 0;
	QByteArray descriptor_, discovered_, held_;
	std::array<rt::RuntimeType, 64> types_{};
	std::array<rt::RuntimeMember, 128> members_{};
	std::array<rt::RuntimeEnumEntry, 32> enums_{};
	std::array<rt::RuntimeCatalog, 18> catalogs_{}; // six groups, three families
	std::array<rt::RuntimeEndpoint, 64> endpoints_{};
	rt::RuntimeSchemaWorkspace metadata_{types_, members_, enums_, catalogs_, endpoints_};
	rt::RuntimeSchema schema_;
	cl::Clock clock_{this, +[](void* self) noexcept {
		                 return static_cast<Check*>(self)->now();
	                 }};
	std::array<std::byte, 546> tx_{}, rx_{};
	telemetry::integration::encoded::EncodedBackend backend_{tx_, rx_, clock_, 30000, true};
	dy::DynamicClient<4, 130, 640> client_{backend_, clock_};
	telemetry::integration::qt::SerialTransport port_;
	QString failure_, identity_;
	QString name_;
	qint32 baud_;
	wire::Capability capability_{};
	std::array<std::uint32_t, 32> ids_{};
	std::array<QByteArray, 32> individual_{};
	std::array<QByteArray, 32> expected_{};
	std::uint32_t unavailable_ = 0, heldSession_ = 0, heldId_ = 0;
	unsigned sent_ = 0;
	bool hold_ = false;

	std::uint32_t now() const noexcept
	{
		return static_cast<std::uint32_t>(elapsed_.elapsed()) + advance_;
	}

	void pump()
	{
		client_.process(now());
		if (!client_.isCurrent() || !port_.canStage())
			return;
		const auto packet = client_.nextPacket();
		if (!packet)
			return;
		require(port_.stage(packet), "Serial stage refused");
		if (!client_.prepareHandoff(packet.session, packet.requestId)) {
			port_.discardStaged();
			throw std::runtime_error("Handoff guard refused");
		}
		++sent_;
		require(port_.sendStaged(), "Serial send refused");
	}

	template<class Predicate>
	void wait(Predicate predicate, int timeout = 20000)
	{
		QElapsedTimer deadline;
		deadline.start();
		while (!predicate()) {
			QCoreApplication::processEvents();
			require(failure_.isEmpty(), qPrintable(failure_));
			pump();
			require(deadline.elapsed() < timeout, "UART check deadline expired");
			QThread::msleep(1);
		}
	}

	wire::Capability discover()
	{
		discovered_.clear();
		failure_.clear();
		port_.connectTo(name_, baud_);
		wait(
		    [&] {
			    return !discovered_.isEmpty();
		    },
		    180000);
		require(port_.ready(), "Incomplete Resource bootstrap");
		const auto capability = wire::parseCapability(input(port_.readManyCapability()));
		require(bool(capability), "Invalid TRM1 record");
		return capability.value;
	}

	void identifyFields()
	{
		// This executable qualifies the named board fixture. Other applications
		// select runtime IDs themselves; no C++ Model or generated DTO is linked.
		unsigned used = 0;
		const auto fieldCatalogs = schema_.catalogs(telemetry::descriptor::Category::Field);
		const auto fields = schema_.endpoints(telemetry::descriptor::Category::Field);
		require(fieldCatalogs.size() == 6 && fieldCatalogs[2].name == "batch32" &&
		            fieldCatalogs[3].name == "batch64" && fieldCatalogs[4].name == "flags" &&
		            fieldCatalogs[5].name == "modes",
		        "Unexpected board fixture catalogs");
		const auto little = [](std::uint64_t value, unsigned width) {
			QByteArray bytes(int(width), '\0');
			for (unsigned i = 0; i < width; ++i)
				bytes[int(i)] = char((value >> (8 * i)) & 255);
			return bytes;
		};
		for (std::size_t row = 0; row < 8; ++row)
			for (std::size_t group = 2; group < fieldCatalogs.size(); ++group) {
				const auto& catalog = fieldCatalogs[group];
				if (row >= catalog.endpointCount)
					continue;
				const auto& field = fields[catalog.firstEndpoint + row];
				if (field.name == "Unavailable") {
					unavailable_ = field.id;
					continue;
				}
				const auto* type = schema_.type(field.typeId);
				require(type && (type->kind == telemetry::TypeKind::Scalar ||
				                 type->kind == telemetry::TypeKind::Enum),
				        "Fixture scalar type");
				require(used < ids_.size(), "Too many fixture Fields");
				const auto bits = group == 2   ? 0x10203040ULL + row * 0x01010101ULL
				                  : group == 3 ? UINT64_MAX - row * 0x0102030405060708ULL
				                  : group == 4 ? std::uint64_t(row % 2 == 0)
				                               : std::uint64_t(std::uint16_t(-2 - int(row)));
				const unsigned width = group == 2 ? 4 : group == 3 ? 8 : group == 4 ? 1 : 2;
				require(type->wireBytes == width, "Fixture wire width mismatch");
				expected_[used] = little(bits, width);
				ids_[used++] = field.id;
			}
		const auto* counter = schema_.findField("device", "Counter");
		require(counter && used == 31 && unavailable_ != 0, "Expected 32 scalar Field IDs");
		ids_[used] = counter->id;
		expected_[used] = little(UINT64_MAX, 8);
	}

	QJsonObject capture(const Completion& result, std::span<const std::uint32_t> ids,
	                    qint64 milliseconds, bool mixed = false)
	{
		require(result.callbacks == 1 && result.success && result.valid,
		        "Batch completion invalid");
		const auto parsed = wire::parseReply(input(result.bytes), ids);
		require(bool(parsed) && parsed.count == ids.size(), "Invalid canonical batch reply");
		unsigned ok = 0, unavailable = 0;
		for (std::size_t i = 0; i < ids.size(); ++i) {
			const auto& item = parsed.items[i];
			if (mixed && item.id == unavailable_) {
				require(item.status == wire::ItemStatus::Unavailable && item.payload.empty(),
				        "Unavailable status lost");
				++unavailable;
				continue;
			}
			require(item.status == wire::ItemStatus::Ok, "Unexpected per-item failure");
			const auto found = std::find(ids_.begin(), ids_.end(), item.id);
			require(found != ids_.end(), "Unexpected fixture ID");
			require(copy(item.payload) == individual_[std::size_t(found - ids_.begin())],
			        "ReadMany differs from individual FieldRead bytes");
			++ok;
		}
		QJsonObject record{{"count", int(ids.size())},
		                   {"passed", true},
		                   {"rawHex", QString::fromLatin1(result.bytes.toHex())},
		                   {"sha256", sha256(result.bytes)},
		                   {"elapsedMs", double(milliseconds)},
		                   {"requestBytes", int(2 + 4 * ids.size())},
		                   {"replyBytes", result.bytes.size()},
		                   {"ok", int(ok)},
		                   {"unavailable", int(unavailable)}};
		return record;
	}

	cl::Submission submit(const rm::ReadManyBinding& binding, Completion& result,
	                      std::uint32_t timeout = 30000)
	{
		require(&result == &completion_, "Completion must use owned storage");
		completion_ = {};
		return rm::readMany(
		    client_, binding,
		    [this](const rm::ReadManyResult& value) noexcept {
			    ++completion_.callbacks;
			    completion_.status = value.operation.status;
			    completion_.success = value.operation.succeeded();
			    completion_.valid = value.valueStatus == dy::ValueStatus::Ok;
			    completion_.bytes = copy(value.operation.payload); // copied inside callback
		    },
		    timeout);
	}

	void load(QJsonObject& report, int seconds)
	{
		if (!seconds)
			return;
		LoadMetrics total;
		std::array<LoadMetrics, 4> metrics{};
		std::array<QJsonObject, 4> samples{};
		QCryptographicHash transcript{QCryptographicHash::Sha256};
		QElapsedTimer duration;
		duration.start();
		const auto sentBefore = sent_;
		qint64 nextProgress = 60000;
		const auto publish = [&](bool passed) {
			auto value = total.json();
			value["schema"] = 1;
			value["pattern"] = "rotating-v1";
			value["requestedSeconds"] = seconds;
			value["elapsedMs"] = double(duration.elapsed());
			value["passed"] = passed;
			value["physicalRequests"] = double(sent_ - sentBefore);
			value["transcriptSha256"] = QString::fromLatin1(transcript.result().toHex());
			QJsonArray rows, last;
			for (std::size_t i = 0; i < loadCounts.size(); ++i) {
				auto row = metrics[i].json();
				row["count"] = int(loadCounts[i]);
				rows.append(row);
				last.append(samples[i]);
			}
			value["byCount"] = rows;
			value["samples"] = last;
			report["load"] = value;
		};
		publish(false);
		try {
			while (duration.elapsed() < qint64(seconds) * 1000) {
				const auto sequence = total.attempted;
				const auto at = std::size_t(sequence % loadCounts.size());
				const auto count = loadCounts[at];
				const auto round = sequence / loadCounts.size();
				const auto start = (round * 7) % ids_.size();
				const auto stride = round % 2 ? 31u : 5u;
				std::array<std::uint32_t, 32> selected{};
				for (unsigned i = 0; i < count; ++i)
					selected[i] = ids_[std::size_t((start + i * stride) % ids_.size())];
				const bool mixed = round % 8 == 7;
				if (mixed)
					selected[count / 2] = unavailable_;
				const auto ids = std::span{selected}.first(count);
				require(client_.pending() == 0, "Load queued an extra exchange");
				const auto binding = rm::bindReadMany(client_, backend_, capability_, ids);
				require(bool(binding), "Load binding failed");
				QElapsedTimer exchange;
				exchange.start();
				require(bool(submit(binding.binding, completion_)), "Load submission failed");
				for (auto* metric : {&total, &metrics[at]})
					metric->accepted(count);
				// An accepted exchange finishes before checking the duration again.
				wait([&] {
					return completion_.callbacks != 0;
				});
				const auto microseconds = quint64(exchange.nsecsElapsed() / 1000);
				for (auto* metric : {&total, &metrics[at]}) {
					metric->callbacks += completion_.callbacks;
					metric->replyBytes += quint64(completion_.bytes.size());
				}
				const auto sample = capture(completion_, ids, qint64(microseconds / 1000), mixed);
				require(client_.pending() == 0 && sent_ - sentBefore == total.attempted,
				        "Load duplicate callback, replay or pending exchange");
				for (auto* metric : {&total, &metrics[at]})
					metric->validated(microseconds, count, mixed ? 1u : 0u);
				samples[at] = sample;
				// Versioned, unambiguous bounded transcript; no per-exchange JSON grows.
				std::array<char, 12> prefix{};
				for (unsigned i = 0; i < 8; ++i)
					prefix[i] = char((sequence >> (8 * i)) & 255);
				for (unsigned i = 0; i < 4; ++i)
					prefix[8 + i] = char((quint32(completion_.bytes.size()) >> (8 * i)) & 255);
				transcript.addData(QByteArrayView{prefix.data(), qsizetype(prefix.size())});
				transcript.addData(completion_.bytes);
				if (duration.elapsed() >= nextProgress) {
					std::printf("ReadMany load elapsedMs=%lld completed=%llu\n",
					            static_cast<long long>(duration.elapsed()),
					            static_cast<unsigned long long>(total.completed));
					std::fflush(stdout);
					nextProgress += 60000;
				}
			}
			for (const auto& metric : metrics)
				require(metric.completed != 0, "Load did not exercise every batch size");
			require(total.attempted == total.completed && total.callbacks == total.completed &&
			            sent_ - sentBefore == total.completed,
			        "Load completion accounting mismatch");
			publish(true);
		} catch (...) {
			publish(false); // Preserve partial counts when a later exchange fails.
			throw;
		}
	}

public:
	Check(QString name, qint32 baud) : name_(std::move(name)), baud_(baud)
	{
		elapsed_.start();
		port_.setReadManyDiscovery(true);
		QObject::connect(&port_, &telemetry::integration::qt::DeviceTransport::descriptorReceived,
		                 &port_, [this](QByteArray bytes) {
			                 discovered_ = std::move(bytes);
		                 });
		QObject::connect(&port_, &telemetry::integration::qt::DeviceTransport::transportError,
		                 &port_, [this](const QString& error) {
			                 failure_ = error;
		                 });
		QObject::connect(&port_, &telemetry::integration::qt::DeviceTransport::packetAccepted,
		                 &port_, [this](quint32 session, quint32 id) {
			                 (void)client_.onSendAccepted(session, id);
		                 });
		QObject::connect(&port_, &telemetry::integration::qt::DeviceTransport::packetReceived,
		                 &port_, [this](QByteArray bytes, quint32 session, quint32 id) {
			                 if (hold_) {
				                 held_ = std::move(bytes);
				                 heldSession_ = session;
				                 heldId_ = id;
			                 } else {
				                 (void)client_.onReply(input(bytes), session, id);
			                 }
		                 });
	}

	~Check()
	{
		client_.close();
		while (!client_.drained())
			client_.process(now());
		port_.close();
	}

	void run(QJsonObject& report, int loadSeconds)
	{
		capability_ = discover();
		descriptor_ = discovered_;
		identity_ = port_.peerAddress();
		const auto parsed = rt::RuntimeSchema::parse(input(descriptor_), metadata_);
		require(bool(parsed), "Descriptor validation failed");
		schema_ = parsed.schema;
		require(capability_.fingerprint == schema_.fingerprint(), "TRM1 fingerprint mismatch");
		require(client_.admit(schema_, input(descriptor_).first(64), capability_.session) ==
		            cl::Status::Ok,
		        "Runtime admission failed");
		identifyFields();
		report["descriptorSha256"] = sha256(descriptor_);
		QJsonArray sessions{double(capability_.session)};
		QElapsedTimer baseline;
		baseline.start();
		for (std::size_t i = 0; i < ids_.size(); ++i) {
			const auto binding = client_.bind(cl::Operation::FieldRead, ids_[i]);
			require(bool(binding), "Individual Field binding failed");
			completion_ = {};
			auto& result = completion_;
			const auto accepted = client_.readField(
			    binding.endpoint, [this](const dy::DynamicResult& value) noexcept {
				    ++completion_.callbacks;
				    completion_.success = value.operation.succeeded() && value.hasValue();
				    completion_.bytes = copy(value.operation.payload);
			    });
			require(bool(accepted), "Individual FieldRead submission failed");
			wait([&] {
				return result.callbacks != 0;
			});
			require(result.callbacks == 1 && result.success, "Individual FieldRead failed");
			individual_[i] = result.bytes;
			require(individual_[i] == expected_[i], "FieldRead differs from fixture wire bits");
		}
		report["individual32ElapsedMs"] = double(baseline.elapsed());
		QJsonArray cases;
		for (const auto count : {8u, 12u, 16u, 32u}) {
			const auto ids = std::span{ids_}.first(count);
			const auto binding = rm::bindReadMany(client_, backend_, capability_, ids);
			require(bool(binding), "ReadMany binding failed");
			auto& result = completion_;
			QElapsedTimer timer;
			timer.start();
			require(bool(submit(binding.binding, result)), "ReadMany submission failed");
			wait([&] {
				return result.callbacks != 0;
			});
			cases.append(capture(result, ids, timer.elapsed()));
		}
		report["cases"] = cases;
		const std::array mixedIds{ids_.front(), unavailable_, ids_.back()};
		const auto mixed = rm::bindReadMany(client_, backend_, capability_, mixedIds);
		require(bool(mixed), "Mixed batch binding failed");
		auto& mixedResult = completion_;
		require(bool(submit(mixed.binding, mixedResult)), "Mixed batch submission failed");
		wait([&] {
			return mixedResult.callbacks != 0;
		});
		report["mixed"] = capture(mixedResult, mixedIds, 0, true);
		load(report, loadSeconds);

		const auto binding =
		    rm::bindReadMany(client_, backend_, capability_, std::span{ids_}.first(8));
		require(bool(binding), "Lifecycle binding failed");
		const auto before = sent_;
		auto& beforeHandoff = completion_;
		require(bool(submit(binding.binding, beforeHandoff, 10)), "Before-handoff submission");
		advance_ += 10;
		client_.process(now());
		require(beforeHandoff.callbacks == 1 && beforeHandoff.status == cl::Status::Timeout &&
		            !client_.recoveryRequired() && sent_ == before,
		        "Before-handoff timeout");

		auto& prepared = completion_;
		hold_ = true;
		require(bool(submit(binding.binding, prepared, 5000)), "Prepared submission failed");
		wait([&] {
			return !held_.isEmpty();
		}); // real board reply, deliberately withheld
		require(prepared.callbacks == 0 && sent_ == before + 1, "Expected one physical request");
		advance_ += 5000;
		client_.process(now());
		require(prepared.callbacks == 1 && prepared.status == cl::Status::Timeout &&
		            client_.recoveryRequired(),
		        "Prepared read timeout must require recovery");
		require(!client_.onReply(input(held_), heldSession_, heldId_), "Late reply was accepted");
		client_.process(now());
		require(prepared.callbacks == 1 && !client_.nextPacket(), "Duplicate callback or replay");
		hold_ = false;
		client_.invalidate();
		client_.process(now());
		require(client_.pending() == 0, "Client not drained");
		const auto oldSession = capability_.session;
		port_.close();
		require(port_.locallyQuiescent(), "TX storage still retained");
		capability_ = discover();
		require(port_.peerAddress() == identity_ && discovered_ == descriptor_,
		        "Peer/schema changed");
		require(capability_.session > oldSession &&
		            capability_.fingerprint == schema_.fingerprint(),
		        "Discovery did not establish a newer admitted session");
		require(client_.confirmQuiescence({oldSession, true, true, true}), "Cleanup proof refused");
		require(client_.admit(schema_, input(descriptor_).first(64), capability_.session) ==
		            cl::Status::Ok,
		        "Fresh admission failed");
		auto& rejected = completion_;
		require(submit(binding.binding, rejected).status == cl::Status::NotAdmitted &&
		            rejected.callbacks == 0,
		        "Old binding survived session change");
		const auto fresh =
		    rm::bindReadMany(client_, backend_, capability_, std::span{ids_}.first(8));
		require(bool(fresh), "New binding failed");
		auto& final = completion_;
		require(bool(submit(fresh.binding, final)), "Post-recovery submission failed");
		wait([&] {
			return final.callbacks != 0;
		});
		(void)capture(final, std::span{ids_}.first(8), 0);
		require(sent_ == before + 2, "Unexpected retry or replay");
		sessions.append(double(capability_.session));
		report["sessions"] = sessions;
		report["capabilityHex"] = QString::fromLatin1(port_.readManyCapability().toHex());
		QJsonObject lifecycle;
		for (const auto* flag :
		     {"preHandoffTimeout", "preparedTimeout", "callbacksOnce", "lateReplyRejected",
		      "reconnect", "staleBindingRejected", "noReplay", "sessionIncreased", "txReleased",
		      "bootstrapComplete", "samePeer"})
			lifecycle[flag] = true;
		report["lifecycle"] = lifecycle;
		report["physicalRequests"] = int(sent_);
		report["passed"] = true;
	}
};
} // namespace

int main(int argc, char** argv)
{
	QCoreApplication application(argc, argv);
	const auto arguments = application.arguments();
	const auto value = [&](const QString& option, const QString& fallback = {}) {
		const auto index = arguments.indexOf(option);
		return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : fallback;
	};
	const auto port = value("--serial");
	const auto path = value("--report");
	bool baudOk = false;
	const auto baud = value("--baud", "115200").toInt(&baudOk);
	bool loadOk = false;
	const auto loadSeconds = value("--load-seconds", "0").toInt(&loadOk);
	QJsonObject report{{"schema", 1},
	                   {"profile", "nucleo-h7s3l8-read-many"},
	                   {"qtVersion", qVersion()},
	                   {"port", port},
	                   {"baud", baud},
	                   {"passed", false}};
	try {
		require(!port.isEmpty() && !path.isEmpty() && baudOk &&
		            baud >= int(telemetry::integration::qt::SerialTransport::minBaudRate()) &&
		            baud <= int(telemetry::integration::qt::SerialTransport::maxBaudRate()),
		        "Usage: telemetry-read-many-uart --serial PORT --baud 115200 --report FILE");
		require(loadOk && loadSeconds >= 0 && loadSeconds <= 1800 &&
		            arguments.count("--load-seconds") <= 1 &&
		            (!arguments.contains("--load-seconds") ||
		             arguments.indexOf("--load-seconds") + 1 < arguments.size()),
		        "--load-seconds requires one integer from 0 through 1800");
		Check check{port, baud};
		check.run(report, loadSeconds);
	} catch (const std::exception& error) {
		report["error"] = QString::fromUtf8(error.what());
	}
	QFile output{path};
	const auto bytes = QJsonDocument(report).toJson();
	if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size())
		return 2;
	std::printf("ReadMany UART passed=%d\n", report["passed"].toBool());
	return report["passed"].toBool() ? 0 : 1;
}

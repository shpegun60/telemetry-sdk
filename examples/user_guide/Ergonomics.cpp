/*
 * @file Ergonomics.cpp
 * @brief Runtime native calls, borrowing, error details and catalog traversal.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/Telemetry.hpp>
#include <cassert>
#include <cstdio>
#include <string_view>

namespace ts = telemetry;

namespace {
unsigned checks = 0;
#define CHECK(expression)                                                                          \
	do {                                                                                           \
		++checks;                                                                                  \
		assert((expression));                                                                      \
	} while (false)

struct Config {
	std::uint32_t period;
};

struct Request {
	std::uint32_t period;
};

struct Reply {
	std::uint32_t period;
};

class Device {
public:
	// Public methods: config(): Borrow settings; configure(): Apply settings;
	// inspect(): Return application result. Device owns all borrowed state.
	const Config& config() const noexcept
	{
		return config_;
	}

	ts::CommandResult configure(const Request& request) noexcept
	{
		if (request.period == 0)
			return ts::CommandResult::InvalidValue;
		config_.period = request.period;
		return ts::CommandResult::Executed;
	}

	ts::ServiceResult<Reply> inspect(const Request& request) const noexcept
	{
		if (request.period == 0)
			return ts::ServiceResult<Reply>::failure(ts::ServiceStatus::Busy);
		return ts::ServiceResult<Reply>::success({config_.period});
	}

private:
	Config config_{10};
};

Device device;
inline constexpr ts::Model model{ts::group("Device", ts::field<&Device::config>("Config", device),
                                           ts::command<&Device::configure>("Configure", device),
                                           ts::service<&Device::inspect>("Inspect", device))};
} // namespace

int main()
{
	// Runtime calls take an ID that arrives at run time; the three endpoints
	// share it here, one per family, in the group's first position.
	const ts::PackedId id = ts::idOf<model, "Device/Config">();
	CHECK(
	    (id == ts::idOf<model, "Device/Configure">() && id == ts::idOf<model, "Device/Inspect">()));
	const auto& fields = model.fields();
	const auto& commands = model.commands();
	const auto& services = model.services();
	const auto config = fields.readBorrowed<Config>(id);
	CHECK(config && config.valueOrNull() == &device.config());
	const auto copy = fields.readAsResult<Config>(id);
	CHECK(copy.status() == ts::FieldReadStatus::Ok && copy.value().period == 10);
	CHECK(fields.readAsResult<double>(id).status() == ts::FieldReadStatus::TypeMismatch);
	CHECK(fields.readAsResult<Config>(0xffff).status() == ts::FieldReadStatus::NotFound);
	const auto command = commands.callAs(id, Request{25});
	CHECK(command.status() == ts::NativeCallStatus::Ok);
	CHECK(command.value() == ts::CommandResult::Executed);
	CHECK(config.value().period == 25 && copy.value().period == 10);
	CHECK(commands.callAs(id, Config{25}).status() == ts::NativeCallStatus::SignatureMismatch);
	const auto reply = services.callAs<ts::ServiceResult<Reply>>(id, Request{1});
	CHECK(reply.hasValue() && reply.value().hasValue());
	CHECK(reply.value().value().period == 25);
	const auto refused = services.callAs<ts::ServiceResult<Reply>>(id, Request{0});
	CHECK(refused.hasValue() && refused.value().status() == ts::ServiceStatus::Busy);
	unsigned entries = 0;
	fields.forEachEntry(
	    [&](ts::PackedId selected, std::string_view group, const ts::FieldEntry& row) {
		    CHECK(selected == id && group == "Device" && std::string_view{row.name} == "Config");
		    ++entries;
	    });
	CHECK(entries == 1);
	unsigned visits = 0;
	CHECK(!fields.forEachWhile([&](std::string_view, const auto&) {
		++visits;
		return false;
	}));
	CHECK(visits == 1);
	static_assert(model.maxCommandRequestWireSize() == ts::wireSize<Request>);
	static_assert(model.maxServiceRequestWireSize() == ts::wireSize<Request>);
	std::printf("Ergonomics guide: %u checks passed\n", checks);
}

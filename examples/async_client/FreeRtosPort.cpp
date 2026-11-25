/* Static FreeRTOS producer/owner integration. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#include "FreeRtosPort.hpp"
#include <algorithm>
#include <cstdlib>

namespace async_example {
StaticMutex::StaticMutex() noexcept : handle_(xSemaphoreCreateMutexStatic(&storage_))
{
	if (!handle_)
		std::abort();
}

StaticMutex::~StaticMutex()
{
	vSemaphoreDelete(handle_);
}

void StaticMutex::lock() noexcept
{
	if (xSemaphoreTake(handle_, portMAX_DELAY) != pdTRUE)
		std::abort();
}

void StaticMutex::unlock() noexcept
{
	if (xSemaphoreGive(handle_) != pdTRUE)
		std::abort();
}

FreeRtosPort::FreeRtosPort(cl::Backend& backend, cl::Clock clock, Hooks hooks) noexcept
    : backend_(backend), clock_(clock), hooks_(hooks), client_(backend, clock),
      producers_(client_, wake, this), done_(xSemaphoreCreateBinaryStatic(&doneStorage_))
{
	if (!done_ || !hooks_.poll || !hooks_.txAvailable || !hooks_.send || !hooks_.reply ||
	    !hooks_.closeAndDrain)
		std::abort();
}

FreeRtosPort::~FreeRtosPort()
{
	if (owner_.load())
		std::abort();
	// A never-started/creation-failed port cannot have accepted requests: only
	// the owner admits a session. Finalize that inert core without callbacks.
	if (!client_.drained()) {
		if (client_.pending() != 0)
			std::abort();
		client_.close();
		client_.process(clock_.now());
	}
	vSemaphoreDelete(done_);
}

bool FreeRtosPort::start(cl::Input header, std::uint32_t generation, UBaseType_t priority) noexcept
{
	if (started_ || header.size() != header_.size() || generation == 0)
		return false;
	started_ = true;
	std::copy(header.begin(), header.end(), header_.begin());
	generation_ = generation;
	admission_.store(cl::Status::Busy);
	const auto task = xTaskCreateStatic(entry, "telemetry", stack_.size(), this, priority,
	                                    stack_.data(), &taskStorage_);
	owner_.store(task);
	if (!task)
		admission_.store(cl::Status::Closed);
	return task != nullptr;
}

cl::Status FreeRtosPort::admissionStatus() const noexcept
{
	return admission_.load();
}

FreeRtosPort::Producers& FreeRtosPort::producers() noexcept
{
	return producers_;
}

cl::Submission FreeRtosPort::requestConfig(
    void (*completed)(const cl::Result<::device_api::device::types::Config>&) noexcept) noexcept
{
	if (!completed)
		return {cl::Status::InvalidArgument, {}};
	return producers_.read(::device_api::device::Config, completed);
}

void FreeRtosPort::wake(void* context) noexcept
{
	const auto owner = static_cast<FreeRtosPort*>(context)->owner_.load();
	if (owner)
		xTaskNotifyGive(owner);
}

void FreeRtosPort::entry(void* context) noexcept
{
	static_cast<FreeRtosPort*>(context)->run();
}

void FreeRtosPort::step() noexcept
{
	hooks_.poll();
	std::size_t bytes = 0;
	std::uint32_t session = 0, request = 0;
	if (hooks_.reply(rx_, bytes, session, request)) {
		if (bytes <= rx_.size())
			(void)client_.onReply(cl::Input{rx_}.first(bytes), session, request);
		else
			backend_.cancel(cl::Status::TransportError);
	}
	client_.process(clock_.now());
	const auto packet = client_.nextPacket();
	if (packet && packet.bytes.size() <= tx_.size() && hooks_.txAvailable(packet.bytes.size())) {
		std::copy(packet.bytes.begin(), packet.bytes.end(), tx_.begin());
		if (client_.prepareHandoff(packet.session, packet.requestId)) {
			if (hooks_.send(cl::Input{tx_}.first(packet.bytes.size()), packet.session,
			                packet.requestId))
				(void)client_.onSendAccepted(packet.session, packet.requestId);
			else
				backend_.cancel(cl::Status::TransportError);
		}
	}
}

void FreeRtosPort::run() noexcept
{
	const auto admitted = client_.admitDescriptor(header_, generation_);
	admission_.store(admitted);
	if (admitted == cl::Status::Ok) {
		const TickType_t pollTicks = std::max<TickType_t>(1, pdMS_TO_TICKS(1));
		for (;;) {
			step();
			if (stopping_.load() && client_.drained())
				break;
			(void)ulTaskNotifyTake(pdTRUE, pollTicks);
		}
	} else {
		client_.close();
		while (!client_.drained())
			client_.process(clock_.now());
	}
	admission_.store(cl::Status::Closed);
	hooks_.closeAndDrain();
	if (xSemaphoreGive(done_) != pdTRUE)
		std::abort();
	// The coordinator deletes this other task after observing done_. It cannot
	// run concurrently on this single-core port; no client/storage use follows.
	for (;;)
		(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void FreeRtosPort::stopAndJoin() noexcept
{
	const auto owner = owner_.load();
	if (!owner)
		return;
	if (owner == xTaskGetCurrentTaskHandle())
		std::abort();
	producers_.close();
	stopping_.store(true);
	wake(this);
	if (xSemaphoreTake(done_, portMAX_DELAY) != pdTRUE)
		std::abort();
	vTaskDelete(owner);
	owner_.store(nullptr);
}
} // namespace async_example

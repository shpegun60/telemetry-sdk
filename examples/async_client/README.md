# Cooperative generated client

`BareMetal.cpp` runs the generated SDK and the same `DeviceClient` used by the
Qt adapter in an ordinary main loop. It has no Qt, HAL, RTOS, mutex or thread
dependency. Generate `device_api` from the example descriptor first (the test
runner does this from the actual native model).

The example addresses the device's Field by path, as every client does:

```cpp
static api::client::Device<api::client::DeviceClient<4, 256>> device{client};
auto one = device.read<"device/Config">(callback, requestTimeoutMs);
const auto deviceConfig = api::client::endpoint<"device/Config">(device); // a handle
auto two = deviceConfig.write(config, callback, requestTimeoutMs);
```

The path is checked at compile time against the generated table, and the
handle forwards to the same `DeviceClient` queue. The third request still uses
the per-family constant `api::device::fields::Config`, which names the same
endpoint; that form is [being removed](../../doc/user/APIOverview.md#виводиться-з-ужитку).
The same operation in the device, TS/JS and the Dynamic client:
[API map](../../doc/user/APIOverview.md#одна-операція-в-чотирьох-місцях).

Implement the `board_*` functions for your board. The application obtains and
fully validates the remote Descriptor, binds it to the peer/session, and passes
its observed 64-byte header through `board_verifiedDescriptorHeader()`. Matching
the generated header is a compatibility check, not peer authentication.

`board_beginPacket()` accepts a complete packet into a transport-owned TX
descriptor. The `portTx` bytes must remain unchanged until the transport releases
them; `board_txAvailable()` reports that release before another copy is made.
Backpressure is checked before handoff authorization. Partial TCP/UART progress
does not count as complete-packet acceptance. RX/DMA IRQs only publish events;
`board_pollReply()` transfers completed frames in the main loop. A Cortex-M7 port
also owns DMA memory accessibility and cache maintenance.

The client owns four stable callback/metadata slots, one 256-byte request pool,
and one generated maximum response DTO scratch. Backend and port TX/RX buffers
are separate and visible in the example. No per-request maximum DTO is reserved.

On shutdown, stop producers, call `close()`, continue the owner loop until
`drained()`, close/drain the physical and remote session, and stop the owner
before destroying storage. With FreeRTOS, one owner task runs the same `step()`;
other tasks may use the optional `ThreadSafeClient` with an RTOS mutex policy.
The mutex wrapper is a task interface; IRQ submission needs a separate ISR ring.

The [qualification runner](../../tests/async_client/run.py) executes this exact
client loop against an actual Model bridge on the host and compiles its
Cortex-M7 object. Host execution and offline ARM compilation do not establish
electrical UART/DMA behavior on a physical board.

## Optional repeated Encoded delivery

Build the same `BareMetal.cpp` with `-DTELEMETRY_EXAMPLE_RETRY_ENCODED` to select
`integration/encoded/RetryingBackend.hpp` and `RetryingEncodedBackend` with
`EncodedRetryOptions{.replyTimeoutMs=500, .maxRetries=3}`. The default remains the
non-retrying EncodedBackend. The qualification runner runs both variants against
the real host Model bridge and compiles both for Cortex-M7.

No board hooks or generated SDK calls change. Three means at most three extra
sends after the initial send, using identical TEQ1 bytes/session/request ID and
fresh handoff permission on each attempt. `board_txAvailable` must retain its
release meaning even when the reply timer expires while DMA is active. Never
overwrite a live transport copy. Report complete-packet acceptance synchronously
with the actual submission, not a delayed acceptance from a previous retry;
physical TX completion is a separate event.

The 500 ms reply budget is an explicit example profile, including complete TX,
server processing, RX and a margin. It is not a measured board limit. Calculate
it from the actual framed byte counts, baud and bounded scheduling delays using
the [UART budget contract](../../doc/AsyncClient.md#uart-reply-budget-and-overall-deadline).
The library's omitted retry options remain 100 ms / three retries. Every example
operation explicitly uses a 30000 ms overall enqueue budget, matching the web
example and including queue wait and all attempts.

The server does not deduplicate Encoded requests. Lost responses can make writes,
commands and services execute again; use only operations that permit this. A
checked response stops future retries, but cannot undo an already delivered copy.
All attempts share the existing enqueue deadline and recovery rules. UART framing
and integrity remain board-owned. No physical H753 run is implied by host/ARM tests.

## Optional FreeRTOS task port

[`FreeRtosPort.hpp`](FreeRtosPort.hpp) and [`FreeRtosPort.cpp`](FreeRtosPort.cpp)
adapt the same generated client to a single-core FreeRTOS application. The
`StaticMutex` policy uses `xSemaphoreCreateMutexStatic`; the owner task and its
stack are created with `xTaskCreateStatic`. Four request slots and one 256-byte
payload pool remain inside `DeviceClient`. Notifications wake the owner; they
carry no request or payload and add no execution queue. The completion semaphore
only joins shutdown.

Construct the port in persistent storage with a borrowed `RpcBackend` or
`EncodedBackend` or `RetryingEncodedBackend`, a millisecond clock safe for all producer tasks, and the five
`Hooks`. The backend, its buffers, callback captures and port storage must outlive
`stopAndJoin()`. `start(observedHeader, generation, priority)` is called once from
a coordinator task. Wait for `admissionStatus()` to leave `Busy`; start producers
only on `Ok`. This observed header comes from the application's fully validated,
peer-bound Descriptor, as in the cooperative example.
If admission fails, the owner closes and drains the empty client before parking;
still call `stopAndJoin()` to delete that task. A never-started port or failed task
creation has admitted no requests; its destructor closes that inert core without
callbacks. Construct and destroy these objects in task context with the scheduler
available for mutex operations.

A producer task can call `port.requestConfig(noexceptCompletionFunction)` or
`port.producers().read/write/command/service(...)`. These return the immediate
submission status, including `QueueFull`; accepted completions run on the owner
task, outside the mutex. Copy any response needed after the callback returns.
The facade is task-only: do not submit, lock, close or invoke callbacks in an ISR.
IRQ handlers publish completed transport events through the board port instead.

The owner waits up to one tick between steps; hooks, callbacks and scheduling
add time. `xTaskNotifyGive` can end that wait earlier. Only the owner touches
the backend or transport. `txAvailable`
checks release/backpressure before the owner copies a complete packet into its
separate 1024-byte TX buffer. `send` reports complete-packet acceptance, never
partial progress; the copied bytes remain leased until `txAvailable` releases
them. A failed handoff is reported conservatively. The port itself does not retry
or add a queue: the selected backend chooses whether to offer a retransmission.
`RetryingEncodedBackend` may execute an Encoded operation repeatedly; the ordinary
EncodedBackend does not retry. Neither automatically reopens an uncertain session.

To stop, first prevent new producer calls and join every producer task, including
any in-flight submission and wake call. Then call `stopAndJoin()` from the
coordinator, never from the owner or its callbacks. It closes admission, wakes
the owner, drains accepted callbacks, and calls `closeAndDrain` on that owner.
That hook must finish/cancel outstanding DMA/TX, close the remote session, and
exclude late frames from it. The owner then signals completion and stops using
the client; the coordinator deletes that other task before returning. Storage
may be destroyed only after this join. This example does not restart a port:
recovery uses a freshly validated session and an explicitly coordinated lifecycle.

Compile the `.cpp` with the generated `device_api` include root, `lib`, and your
actual FreeRTOS/configuration/CMSIS include paths, for example:

```sh
arm-none-eabi-g++ -std=c++20 -O2 -mcpu=cortex-m7 -mthumb \
  -mfpu=fpv5-d16 -mfloat-abi=hard -fno-exceptions -fno-rtti \
  -DTINY_DELEGATE_ENABLE_HEAP_FALLBACK=0 -DSTM32H753xx \
  -Ilib -I"$SDK/cpp" -isystem "$FREERTOS/include" \
  -isystem "$FREERTOS/portable/GCC/ARM_CM4F" -isystem "$BOARD/Core/Inc" \
  -isystem "$BOARD/Drivers/CMSIS/Include" \
  -isystem "$BOARD/Drivers/CMSIS/Device/ST/STM32H7xx/Include" \
  -c examples/async_client/FreeRtosPort.cpp -o "$OUT/FreeRtosPort.o"
```

Choose the portable header directory configured by your actual FreeRTOS kernel;
the local H753 tree uses `GCC/ARM_CM4F`. Static allocation, mutexes,
`vTaskDelete`, `xTaskGetCurrentTaskHandle` and task notifications must be enabled.
The 1024-word owner stack is an example allocation, not a measured stack bound;
size and check it for your callbacks and port. This optional example is compiled
with real local FreeRTOS headers and allocation fallback disabled; scheduler,
priority, DMA/cache and hardware behavior remain application validation work.

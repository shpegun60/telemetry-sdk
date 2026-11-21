# RpcMailbox host integration

[`main.cpp`](main.cpp) is a complete C++20 host program for the optional
[`RpcMailbox`](../../lib/integration/telemetry/v3/RpcMailbox.hpp) provider. It
publishes `/telemetry/rpc.bin` in a normal Resource file array, sends a Service
request through the existing Resource client/server, and decodes its Ready
reply. The [RPC v1.0 contract](../../doc/RpcMailboxV1.md) owns the wire bytes,
cursor, timeout and retry rules.

The Model is one group, and the request carries the Service's ID taken from
its path:

```cpp
inline constexpr auto branch = ts::group("counter", ts::service<&Counter::add>("Add", counter));
inline constexpr ts::Model model{branch};
inline constexpr auto AddId = ts::idOf<model, "counter/Add">(); // group 0, entry 0
```

The example has a small fixed queue and one owner. Ingress attaches peer
identity and `connectionGeneration` to each complete packet. The owner checks
both before `resource::protocol::process()`. The generation belongs to queue
metadata; it is absent from Resource and RPC wire bytes. A disconnect changes
the generation and resets the mailbox. The program then enqueues a delayed
packet from the previous connection, even though the new connection reuses the
same peer and transaction ID. That old packet and a foreign peer are rejected
without touching the provider.

Each owner step first calls `process(nowMs)` and then consumes at most one
packet. A final WRITE therefore hands its ACK to the transport on one step;
its callback can run only on the next step. `HostTransport::send()` copies the
complete reply into separate fixed storage before returning. A real transport
must accept the ACK before the owner advances to dispatch, and keep its TX
storage alive until the send/DMA operation stops using it. This ordering does
not wait for the remote client to receive the ACK.

The checks cover old-generation and foreign-peer rejection, ACK-before-dispatch,
one Service callback, an identical final WRITE retry without a second callback,
and matching transaction/endpoint/operation, state and statuses before decoding
the returned counter. The client starts its READ from zero and uses checked
Resource reply parsers. There are no sockets, UART, HAL, RTOS or MCU measurements
in this example.

Run from the repository root with an installed C++20 compiler. Create the
ignored output directory first; for a shell with `g++` on PATH:

```sh
mkdir -p build/rpc-mailbox-example
g++ -std=c++20 -O2 -Ilib -Ilib/boost_pfr/include -Ilib/magic_enum \
  examples/rpc_mailbox/main.cpp lib/resource/protocol/Protocol.cpp \
  lib/telemetry/model/Adapter.cpp lib/telemetry/abi/StructuredAbi.cpp \
  lib/integration/telemetry/v3/RpcMailbox.cpp \
  -o build/rpc-mailbox-example/rpc_mailbox
build/rpc-mailbox-example/rpc_mailbox
```

For a qmake application, select the independent RPC option before the Integration
manifest. It includes the Telemetry and Resource dependencies:

```qmake
CONFIG += resource_telemetry_rpc
include(path/to/lib/integration/integration.pri)
```

This selects `RpcMailbox.cpp` and generic `Protocol.cpp`, without selecting
`Values.cpp`. Add `resource_telemetry` alongside `resource_telemetry_rpc` when
the application also exposes DescriptorFile/ValuesFile. The Descriptor/Values
option by itself does not compile RPC.

The maintained [RPC checks](../../tests/structured/rpc/README.md) also compile
and run this example:

```sh
python tests/structured/rpc/run.py --cxx g++ --build-dir build/rpc-gcc
```

On Windows select the installed Qt MinGW compiler and put its `bin` directory
first in PATH. `RPC_ARM_PROBE` suppresses host output for offline ARM
compile/link checks; it does not turn those checks into hardware execution.

Application integration must also provide descriptor fingerprint agreement
before admission, real frame assembly, bounded ingress/backpressure, a clock,
owner/data synchronization, and generation values that cannot repeat while an
old frame might still arrive. Clearing a queue alone cannot reject a delayed
ingress callback. Stamp packets from the ingress connection context, never from
the currently admitted generation at dequeue.

`complete=true` seals a request; a disconnect/reset can discard Pending before
dispatch. After a disconnect the client can have an unknown outcome: execution
may have happened before the result was lost. Retrying a non-idempotent operation
in a new session can repeat its effect. Resolve that outcome in the application;
mailbox deduplication applies only within its retained admitted session.

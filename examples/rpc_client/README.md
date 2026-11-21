# Typed asynchronous RPC client

[Client contract](../../doc/RpcClientV1.md) · [Server wire](../../doc/RpcMailboxV1.md) ·
[Several clients](../rpc_multi_session/README.md)

[`main.cpp`](main.cpp) is a complete C++20 program using the real `RpcClient`,
Resource packet processor and `RpcMailbox`. It writes/reads a Field, calls a
Command, and calls a typed `Add -> Report` Service. The server's Model is one
group, `demo`, holding the three side by side. Endpoint IDs are packed u32
values taken from the paths (`ts::idOf<model, "demo/Add">()`); each family
numbers its own entries, so all three are 0. An explicit application handshake rejects a simulated offered
fingerprint mismatch before admission. Matching fingerprints admit the shared
endpoint/schema mapping; the example does not fetch a remote descriptor.

The client borrows three arrays: request bytes, result payload and one outgoing
Resource packet. The example also creates a server for a local byte loopback;
a remote client needs neither that Model nor its mailbox. There is no network,
RTOS, heap or implicit blocking wait inside `RpcClient`.

The bounded `complete()` helper simulates an event loop. Each iteration supplies
the current monotonically advancing u32 millisecond tick, borrows `nextPacket()`,
confirms full send acceptance, delivers the matching complete Resource reply,
then runs the mailbox. The final WRITE ACK is therefore handled before callback
execution. Replace those handoffs with your framed UART/TCP queue, preserving
generation/requestId metadata and the lifetime of outgoing bytes.
The fake transport drops one final WRITE ACK and checks that the client repeats
exactly the same packet/ticket without a second application callback. Short
example deadlines keep that simulation bounded; production defaults differ.

```cpp
constexpr ::telemetry::client::rpc::ServiceEndpoint<Add, Report> add{ts::idOf<model, "demo/Add">(), ::telemetry::client::rpc::trustedRpcMapping};
client.startSession(1); // after application schema agreement/admission
client.beginService(add, Add{7});
// Feed time, send acceptance and replies from your application event loop.
Report report{};
if (client.completed()) {
    const auto decoded = client.decodeResult(report);
    // Inspect decoded and client.diagnostic() before using report.
}
```

`Completed` includes valid remote refusals. Inspect client, Resource, dispatch
and operation status independently. A lost connection after possible final
acceptance can produce `OutcomeUnknown`; reconnect never replays that operation
automatically. Bare Resource replies contain no request ID: serialize exchanges
on one channel or provide pairing in outer framing.

From the repository root, with a C++20 compiler on PATH:

```sh
mkdir -p build/rpc-client-example
g++ -std=c++20 -O2 -Ilib -Ilib/boost_pfr/include -Ilib/magic_enum \
  examples/rpc_client/main.cpp lib/resource/protocol/Protocol.cpp \
  lib/client/rpc/RpcClient.cpp \
  lib/integration/telemetry/v3/RpcMailbox.cpp \
  lib/telemetry/model/Adapter.cpp lib/telemetry/abi/StructuredAbi.cpp \
  -o build/rpc-client-example/rpc_client
build/rpc-client-example/rpc_client
```

The maintained [client runner](../../tests/structured/rpc_client/README.md)
compiles and executes this example and checks its measured condition count.
Its loopback requires server sources; a real **client-only** qmake project uses:

```qmake
CONFIG += telemetry_client_rpc
include(path/to/lib/client/client.pri)
```

That selects only `RpcClient.cpp` and header-only Codec dependencies, without
`RpcMailbox.cpp`, Model Adapter or StructuredAbi. For a project also serving
RPC, select `resource_telemetry_rpc` and include
`lib/integration/integration.pri` for the server and its dependencies.

The full [buffer, retry, type and lifetime rules](../../doc/RpcClientV1.md)
also cover large caller-owned results and Workspace leases. Host loopback and
offline ARM compile/link checks are distinct from execution on hardware.

# Four RPC clients and a bounded server pool

[Session contract](../../doc/RpcSessionsV1.md) · [Client contract](../../doc/RpcClientV1.md)

[`main.cpp`](main.cpp) runs four real `RpcClient` instances against four stable
`RpcMailbox` objects in `RpcSessionPool<4>`. Clients A/B use independent simulated
connections; C/D use distinct channels on a shared connection with explicit
outer frame pairing. All mailboxes share one Model and serialized Workspace,
while their request/result arrays remain separate. The Model is one group,
`ts::group("counter", ts::service<&Counter::add>("Add", counter))`, and the
clients' `ServiceEndpoint` takes its ID from the path:
`ts::idOf<model, "counter/Add">()`.

The host example checks concurrent transaction ID 1, different results, one
lost WRITE ACK followed by an identical retry, and release/reacquire followed
by rejected old packets and reply tickets. There are five application callbacks:
four initial calls and one call after reconnect. A retry adds no callback.

The complete-packet handoff carries trusted connection generation/channel and
the local request ticket outside Resource/RPC bytes. One owner performs:

1. Tick the pool and clients with current time.
2. Reject ingress from a released connection generation.
3. Pass a complete request to `sessions.handle(handle, request, reply)`.
4. Hand the full ACK to the transport, then confirm its exact pool reply ticket.
5. Let a later bounded `sessions.process(nowMs, budget)` dispatch Pending work.

`replyAccepted()` confirms transport handoff, not receipt at the remote client.
The transport preserves its TX storage until sending is finished. No raw
Resource reply can identify a channel by itself; real multiplexing must retain
this outer association. A TCP stream also needs framing before `handle()`.

Admission, queues, sockets, RTOS synchronization and device configuration belong
to application. Pool capacity is fixed; full/Closing/exhausted slots are never
silently evicted. Release invalidates handles immediately; Running reset waits
for the callback to return. Referents returned by borrowed getters/services
remain protected until encoding finishes.

Build and run from the repository root:

```sh
mkdir -p build/rpc-multi-session-example
g++ -std=c++20 -O2 -Ilib -Ilib/boost_pfr/include -Ilib/magic_enum \
  examples/rpc_multi_session/main.cpp lib/resource/protocol/Protocol.cpp \
  lib/client/rpc/RpcClient.cpp \
  lib/integration/telemetry/v3/RpcMailbox.cpp \
  lib/telemetry/model/Adapter.cpp lib/telemetry/abi/StructuredAbi.cpp \
  -o build/rpc-multi-session-example/rpc_multi_session
build/rpc-multi-session-example/rpc_multi_session
```

For qmake:

```qmake
CONFIG += telemetry_client_rpc resource_telemetry_rpc_sessions
include(path/to/lib/client/client.pri)
include(path/to/lib/integration/integration.pri)
```

Descriptor/Values can be added independently with `resource_telemetry`.
The [session runner](../../tests/structured/rpc_sessions/README.md) compiles and
executes this program under maintained host modes. Its four clients are a
functional example, not a latency measurement or a hardware qualification.

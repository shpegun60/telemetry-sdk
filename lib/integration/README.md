# Optional integrations

Author: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

Integration connects the independent Telemetry, Resource and Client modules.
Select the adapter needed by the application; portable models and queues do not
select a transport, Qt, an operating system or a second execution queue.

## Choose an adapter

Headers below are included using the shown paths. Their implementation files
remain under `lib/integration`.

| CMake component | Public header | Direct component dependencies | Use |
| --- | --- | --- | --- |
| `dynamic_client` | `integration/dynamic/DynamicClient.hpp` | `runtime`, `client` | Runtime endpoint checks over the existing DeviceClient FIFO |
| `read_many_client` | `integration/read_many/Client.hpp` | `dynamic_client`, `encoded_wire` | Optional scalar Encoded batch over that same FIFO |
| `read_many_static_client` | `integration/read_many/StaticClient.hpp` | `client`, `encoded_wire` | Typed scalar ReadMany without RuntimeSchema over the existing FIFO |
| `batch_client` | `integration/batch/Client.hpp` | `dynamic_client`, `encoded_wire` | Optional ordered mixed operations over the same FIFO |
| `batch_static_client` | `integration/batch/StaticClient.hpp` | `client`, `encoded_wire` | Typed mixed operations with caller-owned DTO storage over the same FIFO |
| `batch_server` | `integration/encoded/BatchDispatcher.hpp` | `core`, `encoded_wire`, `encoded_server` | Global preflight and independent per-item outcomes for Mixed Batch |
| `read_many_workbench` | `integration/read_many/Workbench.hpp` | `workbench`, `read_many_client` | Background read values without replacing the selected snapshot or draft |
| `inspector` | `integration/inspector/InspectorModel.hpp` | `dynamic_client` | Read-only discovery and one owned selected-Field snapshot |
| `workbench` | `integration/workbench/WorkbenchModel.hpp` | `inspector` | Fixed-storage Field editing and one Write/Refresh workflow on the same FIFO |
| `encoded_backend` | `integration/encoded/Backend.hpp`, `integration/encoded/RetryingBackend.hpp` | `client`, `encoded_wire` | Client-side TEQ1 packets, with optional explicit retries |
| `rpc_backend` | `integration/rpc/Backend.hpp` | `client`, `rpc_client` | Adapt the existing one-mailbox RPC engine to DeviceClient |
| `encoded_server` | `integration/encoded/Dispatcher.hpp` | `core`, `encoded_wire` | Validate a TEQ1 envelope and invoke the existing core dispatcher |
| `resource_adapter` | `integration/telemetry/v3/DescriptorFile.hpp`, `integration/telemetry/v3/ValuesFile.hpp` | `core`, `resource` | Publish Descriptor and live Values through the file API |
| `rpc_server` | `integration/telemetry/v3/RpcMailbox.hpp` | `core`, `resource`, `rpc_wire` | One deferred RPC mailbox provider |
| `rpc_sessions` | `integration/telemetry/v3/RpcSessionPool.hpp` | `rpc_server`, `resource_protocol` | Independently owned mailboxes and Resource packet routing |
| `tcp_server` | `integration/tcp/Server.hpp` | `encoded_server`, `resource_protocol` | Frame Resource/TEQ1 streams in caller-owned storage |
| `qt_transport` | `integration/qt/DeviceTransport.hpp`, `DescriptorBootstrap.hpp` | `client`, `resource_wire`, `encoded_wire`, Qt Core | Optional host transport boundary and sequential Descriptor discovery |
| `qt_serial_transport` | `integration/qt/SerialTransport.hpp` | `qt_transport`, Qt SerialPort | Optional UART adapter with privately built COBS framing |

Client adapters, the runtime client, Inspector and TCP integration use
`telemetry::integration::<adapter>`. The Descriptor/Values/RPC file providers
use `resource::telemetry::v3`: this is an extension implemented here, and does
not add Telemetry knowledge to `lib/resource`. The shared byte contracts are in
`lib/wire`; native RPC client code is in `lib/client/rpc`.

The TCP component contains no socket implementation. The reusable Qt serial
adapter belongs to the optional SDK components described in
[Qt transport](../../doc/QtTransport.md). Socket implementations, serial-port
selection and GUI controllers belong to the application; maintained
examples are [Inspector QML](https://github.com/shpegun60/telemetry-ux/blob/main/examples/inspector/README.md),
[COBS integration](../../examples/cobs_integration/README.md) and
[NUCLEO-H7S3L8](../../examples/nucleo_h7s3l8/README.md).

## CMake

For an installed package, select both the client facade and a concrete backend:

```cmake
find_package(Telemetry REQUIRED COMPONENTS dynamic_client encoded_backend)
target_link_libraries(my_app PRIVATE
    Telemetry::dynamic_client Telemetry::encoded_backend)
```

For a source checkout, select the same components before adding the repository:

```cmake
set(TELEMETRY_COMPONENTS "dynamic_client;encoded_backend")
add_subdirectory(path/to/telemetry telemetry-build)
target_link_libraries(my_app PRIVATE
    Telemetry::dynamic_client Telemetry::encoded_backend)
```

Dependencies in the table are selected automatically. `core` is needed when the
application owns a native Model; a remote-only DynamicClient does not need it.
Use `inspector` instead of `dynamic_client` for the read-only browser facade.
The default root installation includes Inspector, Workbench and TCP; explicit component
selection keeps smaller applications independent. Installed targets export only
their selected component headers and dependencies.

## qmake

Set selection flags before including `integration.pri`:

| Adapter | Selection |
| --- | --- |
| Encoded/Retrying Encoded backend | `CONFIG += telemetry_encoded_backend` |
| RPC backend | `CONFIG += telemetry_rpc_backend` |
| TEQ1 server dispatcher | `CONFIG += resource_telemetry_encoded` |
| Descriptor and Values providers | `CONFIG += resource_telemetry` |
| RPC mailbox provider | `CONFIG += resource_telemetry_rpc` |
| RPC session pool | `CONFIG += resource_telemetry_rpc_sessions` |
| Native TCP stream/server | `CONFIG += telemetry_tcp_server` |

DynamicClient and Inspector have direct manifests. For example:

```qmake
CONFIG += telemetry_encoded_backend
include(path/to/telemetry/lib/integration/dynamic/dynamic.pri)
include(path/to/telemetry/lib/integration/integration.pri)
```

For Inspector, replace `dynamic/dynamic.pri` with `inspector/inspector.pri`.
For FieldWrite Workbench, use `workbench/workbench.pri` and select a backend in
the same way. The existing Inspector API remains read-only.
For the native RPC engine without DeviceClient, select
`CONFIG += telemetry_client_rpc` and include `lib/client/client.pri`.
Repeated inclusion of these manifests does not duplicate compiled sources.

Mixed Batch selects `CONFIG += telemetry_batch_client` or `telemetry_batch_server`
through `integration.pri`. Its separate TBM1 capability enables operation 6;
TRM1 remains read-only. See [Mixed Batch](../../doc/MixedBatchEncoded.md) for
buffer budgets, ordered execution and explicit retry semantics.

qmake uses the shared source include root and its Resource manifest includes the
generic packet processor. CMake provides finer component and header isolation;
the functional responsibilities are the same in both build systems.

## Ownership and completion

The application owns transport buffers, schema storage and the owner loop.
DynamicClient uses the existing DeviceClient FIFO. Inspector copies a response
before its borrowed callback ends and retains one selected snapshot. A changed
schema/session requires invalidation, drain and actual transport/peer cleanup.
Queue emptiness alone does not prove physical TX/DMA release.

See [module boundaries](../../doc/Modules.md),
[DynamicClient usage](../../doc/DynamicClientUsage.md),
[Inspector ownership](../../doc/InspectorModel.md),
[asynchronous client and transport](../../doc/AsyncClient.md) and
[RPC sessions](../../doc/RpcSessionsV1.md) for the complete contracts.

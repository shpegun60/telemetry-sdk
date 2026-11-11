# Independent modules

Telemetry has five functional modules. They remain in one repository and can
be selected, built and installed independently.

CMake components are smaller selectable build units within those five modules;
they do not introduce additional runtime layers. The
[Integration entry point](../lib/integration/README.md) maps adapters to public
headers, dependencies and CMake/qmake selections.

| Module | Sources | Responsibility |
| --- | --- | --- |
| Telemetry | `lib/telemetry` | Definitions, iteration, Native, Encoded, Codec, descriptor construction and runtime wire inspection |
| Resource | `lib/resource` | Files, providers and the generic file protocol |
| Client | `lib/client` | Async requests, queue, callbacks, deadlines, cancellation, shutdown and native RPC packet engine |
| Integration | `lib/integration` | Optional Resource providers, server dispatchers and client backends |
| Generator | `tools/telemetry_codegen` | Host tool converting a Descriptor into an SDK |

`lib/common` supplies the existing fixed-storage callable utilities through
`tiny_delegate`. `lib/wire` supplies the shared TEQ1 and RPC byte contracts.
Neither utility requires a Model, a filesystem, a client or an operating system.
Vendored dependencies keep their existing pinned contents and licenses.

Telemetry and Resource do not include or link to each other or to Client.
Client core uses the standard library and common callable utilities. Generated
ClientBindings supplies standalone DTO codecs; a local Model is unnecessary.
Native RPC is an optional Client component with native Codec and Resource
packet dependencies. The native C++ backends belong to Integration.

The portable JS/TS runtime in `lib/client/web` remains a single distribution
containing DeviceClient and its Encoded/RPC adapters. The generator copies it
only when `--client-runtime` is selected. The C++ source and target boundaries
described here do not imply separate JS packages; generated standalone web
codecs do not require that runtime.

The separate [browser Descriptor helpers](../web/README.md) under `web` provide
runtime schema parsing and payload codecs for the structured browser example.
They do not own a request FIFO; asynchronous JS/TS operations use `lib/client/web`.

| API | Canonical include | Namespace |
| --- | --- | --- |
| Local Model | `telemetry/Telemetry.hpp` | `telemetry` |
| Protocol-free dispatcher | `telemetry/encoded/Dispatcher.hpp` | `telemetry` |
| Descriptor builder/reader | `telemetry/descriptor/Descriptor.hpp` | `telemetry::descriptor` |
| Runtime schema and wire views | `telemetry/runtime/DynamicValueView.hpp` | `telemetry::runtime` |
| Transactional wire edits | `telemetry/runtime/DynamicValueWriter.hpp` | `telemetry::runtime` |
| Optional Workbench Watchlist reads | `integration/read_many/Workbench.hpp` | `telemetry::integration::read_many` |
| Generic files | `resource/Resource.hpp` | `resource` |
| Portable client | `client/DeviceClient.hpp` | `telemetry::client` |
| Native RPC engine | `client/rpc/RpcClient.hpp` | `telemetry::client::rpc` |
| Shared TEQ1/RPC contracts | `wire/EncodedWire.hpp`, `wire/RpcWire.hpp` | `telemetry::protocol::encoded`, `telemetry::protocol::rpc` |
| Device identity file (`/telemetry/identity.bin`) | `wire/IdentityWire.hpp` | `telemetry::protocol::identity` |
| Resource providers | `integration/telemetry/v3/DescriptorFile.hpp`, `ValuesFile.hpp`, `IdentityFile.hpp`, `RpcMailbox.hpp`, `RpcSessionPool.hpp` | `resource::telemetry::v3` |
| TEQ1 server | `integration/encoded/Dispatcher.hpp` | `telemetry::integration::encoded` |
| Encoded backend | `integration/encoded/Backend.hpp` | `telemetry::integration::encoded` |
| Retrying Encoded backend | `integration/encoded/RetryingBackend.hpp` | `telemetry::integration::encoded` |
| RPC backend | `integration/rpc/Backend.hpp` | `telemetry::integration::rpc` |
| Runtime client | `integration/dynamic/DynamicClient.hpp` | `telemetry::integration::dynamic` |
| Optional ReadMany | `integration/read_many/Client.hpp` | `telemetry::integration::read_many` |
| Optional Mixed Batch client | `integration/batch/Client.hpp` | `telemetry::integration::batch` |
| Optional Mixed Batch Workbench bridge | `integration/batch/Workbench.hpp` | `telemetry::integration::batch` |
| Optional Mixed Batch server | `integration/encoded/BatchDispatcher.hpp` | `telemetry::integration::encoded` |
| Read-only Inspector Model | `integration/inspector/InspectorModel.hpp` | `telemetry::integration::inspector` |
| FieldWrite, Command and Service Workbench | `integration/workbench/WorkbenchModel.hpp` | `telemetry::integration::workbench` |
| Native TCP framing/server | `integration/tcp/Server.hpp` | `telemetry::integration::tcp` |
| Optional combined TCP capabilities | `integration/tcp/EncodedProfile.hpp` | `telemetry::integration::tcp` |
| Optional Qt transport/bootstrap | `integration/qt/DeviceTransport.hpp`, `DescriptorBootstrap.hpp` | `telemetry::integration::qt` |
| Optional Qt UART transport | `integration/qt/SerialTransport.hpp` | `telemetry::integration::qt` |

The Resource provider namespace denotes an extension implemented by Integration;
generic Resource contains no Telemetry headers or providers. The descriptor
builder is independent of Resource read results. DescriptorFile performs the
actual result mapping at the file boundary. Values retains its existing wire
format and sampling behavior.

Old Client and Resource/Telemetry include trees and compatibility facades are
removed. Applications must update their includes and client/transport namespaces
and rebuild their C++ modules. Descriptor v3.0, RPC v1.0, TEQ1 and Resource bytes
remain unchanged. Native endpoint definitions and DeviceClient behavior remain
unchanged.

## CMake

```sh
cmake -S . -B /tmp/telemetry-core -DTELEMETRY_COMPONENTS=core \
  -DCMAKE_INSTALL_PREFIX=/tmp/telemetry-install
cmake --build /tmp/telemetry-core
cmake --install /tmp/telemetry-core
```

Downstream projects use the installed package:

```cmake
find_package(Telemetry REQUIRED COMPONENTS core)
target_link_libraries(application PRIVATE Telemetry::core)
```

Available components are `core`, `runtime`, `resource`, `resource_protocol`, `client`,
`codec`, `resource_adapter`, `encoded_server`, `rpc_server`, `rpc_sessions`,
`encoded_backend`, `rpc_client`, `rpc_backend`, `dynamic_client`, `read_many_client`, `read_many_static_client`, `batch_client`, `batch_static_client`, `batch_server`, `inspector`, `workbench`, `read_many_workbench` and
`tcp_server`, `tcp_encoded_profile`, `batch_workbench`, `files_client`, `files_workbench`, `link`, `tcp_link`, `qt_connection`,
`qt_tcp_transport`, `qt_transport` and `qt_serial_transport`. Shared header components
`common`, `delegate`, `reflection`, `resource_wire`, `encoded_wire` and `rpc_wire`
are selected automatically as needed. Multiple components use a semicolon list.
The native RPC engine is `rpc_client`; its DeviceClient adapter is `rpc_backend`.
The default root build includes the Inspector, Workbench and TCP components. Set
`TELEMETRY_COMPONENTS` explicitly to build and install a smaller selection.
The two Qt components are excluded from that default. `qt_transport` requires
Qt Core; `qt_serial_transport` adds Qt SerialPort and builds the existing COBS
implementation privately. Installed serial consumers need Qt and the SDK prefix,
without a COBS checkout. Select Qt components explicitly in downstream
`find_package(Telemetry ... COMPONENTS ...)`; requesting only `client` does not
search for Qt, even when the prefix also contains transport libraries. See
[Qt transport](QtTransport.md) for source and installed builds.
`rpc_server` supplies the mailbox provider with `core`, `resource` and `rpc_wire`;
it does not compile the generic Resource packet processor. `rpc_sessions` adds
`resource_protocol` because the session pool actually routes Resource packets.
The header-only `runtime` component inspects Descriptor v3 and canonical payloads
and stages transactional edits of those values.
It has no dependency on reflection, Model, Codec, Resource, Client or Integration.
Its public types share only the fixed wire kinds and limits used by native types.
See [Runtime schema](RuntimeSchema.md) for workspace and borrowed-buffer lifetimes.
The optional `dynamic_client` component binds `runtime` to `client`, privately
using the existing FIFO. It includes no Model, wire parser, Resource or Qt;
choose a transport backend separately. See [Using DynamicClient](DynamicClientUsage.md).
The optional `read_many_client` adds the Encoded scalar batch mapping over the
same private FIFO, without changing the base target's include closure. It adds
only `encoded_wire` to `dynamic_client`; select a concrete enabled Encoded backend
separately. See [ReadMany](ReadManyEncoded.md).
The optional `read_many_static_client` provides typed scalar ReadMany for generated
or handwritten Codec policies. Its only direct dependencies are `client` and
`encoded_wire`, with no RuntimeSchema, Model or UI headers. See
[Typed ReadMany](StaticReadMany.md).
The optional `batch_client` adds ordered FieldRead/FieldWrite/Command/Service
requests over that same FIFO. `batch_server` selects the transport-independent
executor and its explicit Encoded admission mapping; selecting `core` alone
does not link either Batch implementation. See [Mixed Batch](MixedBatchEncoded.md).
The optional `batch_static_client` provides the same ordered operations with
typed requests and caller-owned response slots. It depends only on `client` and
`encoded_wire`, without RuntimeSchema or native Model headers. See
[Typed Mixed Batch](StaticBatch.md).
The optional `read_many_workbench` combines `workbench` and `read_many_client`
for snapshot-preserving background reads on that same FIFO. It adds no storage
to the base Workbench. See [Scalar Watchlist](ReadManyWatchlist.md).
The optional `workbench` component reuses Inspector ownership and the same client
FIFO for transactional Field edits, Write/Refresh, Command execution and Service calls.
Its shared request editor has no independent scheduler or storage allocation.
Service responses use an explicitly supplied caller-owned buffer and the same
SnapshotStore implementation as Field snapshots.
It has no Qt, native Model or concrete backend dependency. See [Workbench](Workbench.md).
The optional `inspector` component adds a bounded, owned snapshot and lazy value
navigation to `dynamic_client`. It lists all endpoint categories but issues only
FieldRead operations. It has no Qt, Resource, native Model or concrete transport
dependency. See [Inspector Model](InspectorModel.md) for ownership and lifecycle.
The optional `tcp_server` component frames existing Resource and TEQ1 packets
into caller-owned storage and invokes the existing dispatchers. Its dependencies
are `resource_protocol` and `encoded_server`; it creates no socket, task, heap
allocation or execution queue. See [TCP integration](../lib/integration/tcp/README.md).
`encoded_backend` exports both `EncodedBackend` and the optional
`RetryingEncodedBackend`; including the latter adds no server, Resource, Qt or
native Model dependency. Both share `client` and `encoded_wire` dependencies.

Each target exports only its own headers and declared dependencies. Neither
in-tree targets nor installed exports grant access to the entire `lib` tree.
Each installed header directory is owned by its component; upgrading removes
obsolete headers from that directory. Installation prefixes are relocatable.
Before cleanup, CMake resolves existing path ancestors and requires the real
destination to remain inside its build directory or installation prefix.
Internal symlinks are supported; intermediate links escaping those roots are
refused, including when the destination header directory does not yet exist.
Windows junctions are expanded component by component on the build host because
some CMake versions leave them unresolved in `REAL_PATH`. Dangling/cyclic links,
unsupported Windows device targets and more than 64 link expansions are refused.
Cleanup still unlinks an owned leaf link without deleting its target.
Install destination directories are normalized before GNUInstallDirs derives
paths and exports. The install script also normalizes runtime `--prefix` and
nonempty DESTDIR before cleanup or copying, so `symlink/..` cannot make those
operations use different directories. Relative runtime prefixes resolve against
the installation invocation directory; CMake's stripped root-prefix sentinel
retains root semantics. Root installation tests use an external POSIX DESTDIR.

Build and installation output roots must be trusted directories whose path
components are not concurrently replaced by another process. Path validation
and filesystem cleanup are separate operations; they do not provide an atomic
guarantee against concurrent symlink or junction substitution.

## qmake

Each module owns one manifest beside its sources: `telemetry.pri`,
`resource.pri`, `client.pri`, `integration.pri`, `common.pri` and `wire.pri`.
The header-only native Codec submodule owns `telemetry/codec/codec.pri`.
The independent runtime module owns `telemetry/runtime/runtime.pri`.
The runtime client and Inspector have direct entry points
`integration/dynamic/dynamic.pri` and `integration/inspector/inspector.pri`.
Include one of those when needed, then select a backend separately; they do not
require additional CONFIG flags.

Select Integration explicitly. Existing selection names `resource_telemetry`,
`resource_telemetry_rpc`, `resource_telemetry_rpc_sessions` and
`resource_telemetry_encoded` select the corresponding providers/server. Use
`telemetry_encoded_backend` or `telemetry_rpc_backend` for client adapters.
The Encoded flag includes the optional retry header too; no extra qmake flag is needed.
`telemetry_client_rpc` selects only the native RPC packet engine.
`telemetry_tcp_server` selects the native TCP stream/server adapter; the application
still owns its socket and connection lifecycle.

```qmake
CONFIG += telemetry_encoded_backend
include(path/to/telemetry/lib/integration/integration.pri)
```

Repeated inclusion does not duplicate compiled sources. The generic Resource
manifest has no Telemetry selections. It selects generic providers and the
Resource packet processor together; use CMake's separate `resource` component
when that distinction is required. CMake's per-component include isolation does
not apply to qmake's shared source include root. The Generator remains a Python
host tool;
standalone generated codecs have no Client, Resource, Qt or Model dependency.

## Verification

`tests/modules/run.py` builds isolated components, installs and relocates them,
then builds independent downstream projects. Positive controls accompany
expected include and link rejections. The suite checks removed headers on
reconfigure/upgrade and refuses source changes during qualification. `--arm`
compiles and links Cortex-M7 images without executing them.
Cleanup controls cover escaping symlinks with unchanged canaries, allowed
internal/leaf links, fresh output, prefix overrides and POSIX DESTDIR staging.
Windows hosts that cannot create symlinks record that platform limitation.
Junction controls remain mandatory on Windows and need no symlink privilege;
synthetic UNC conversion checks do not access network shares.

Focused dispatcher, descriptor, client, SDK and wire regressions cover preflight,
overlap, error precedence, family statuses and exact bytes. Runtime sanitizers
and the existing CI matrix remain required. All build outputs and receipts
belong outside the source repository. Historical hardware evidence retains its
original source identity; this refactoring does not establish new board results.

## Optional owned Qt connection

`qt_connection` exports [ConnectionSession](QtConnectionSession.md), one host
owner/FIFO and its Resource/Encoded TCP reference profile. `qt_tcp_transport`
exports only the Qt Network carrier; `tcp_wire` holds framing without native
server dependencies. A host transport is a link plus a framer: `link` holds the
Qt-free `Framer` contract, `tcp_link` the TCP profile's framer, and
`qt_transport` the `Link` channel and `LinkTransport`, which carries discovery
and exchange for any medium. `TcpTransport` is that transport over a `TcpLink`;
[tests/link_transport](../tests/link_transport/README.md) proves the split
changed nothing it does. These components are explicit opt-ins and are absent from
the portable default selection. QML/HTML presentation remains in Telemetry UX.

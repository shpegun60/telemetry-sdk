# Runtime schema and dynamic wire values

The C++20 runtime module consumes Descriptor v3.0 bytes without knowing a
Model's native C++ types. `RuntimeSchema` indexes validated types, structure
members, enum entries, catalogs and endpoints in caller-owned storage.
`DynamicValueView` borrows canonical payload bytes and reads scalar leaves;
it does not construct a native aggregate or a tree of values.
`DynamicValueWriter` stages checked scalar edits in caller-owned wire storage
and commits a complete encoded value.

These types live in `telemetry::runtime`. They use the same fixed wire kinds,
limits and descriptor constants as the producer, without native reflection,
Resource, Client, Integration, Qt or an operating system. Remote runtime
requests use the separate [DynamicClient integration](DynamicClientUsage.md)
and its existing DeviceClient FIFO; the runtime module itself schedules no
requests. Existing Descriptor v3.0, TEQ1 and RPC wire contracts are unchanged.

## Read an unknown value

Include `<telemetry/runtime/DynamicValueView.hpp>`. The schema-only header is
`<telemetry/runtime/RuntimeSchema.hpp>`. Supply arrays sized for the application's
schema budget; parsing never grows them or allocates hidden storage.

```cpp
namespace rt = telemetry::runtime;
std::array<rt::RuntimeType, 64> types{};
std::array<rt::RuntimeMember, 256> members{};
std::array<rt::RuntimeEnumEntry, 128> enums{};
std::array<rt::RuntimeCatalog, 32> catalogs{};
std::array<rt::RuntimeEndpoint, 128> endpoints{};
rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};

auto parsed = rt::RuntimeSchema::parse(descriptorBytes, workspace);
if (!parsed) {
    // Inspect parsed.status, parsed.error and parsed.requirements.
    return;
}
const auto* field = parsed.schema.findField("device", "Config");
if (!field)
    return;
auto handle = parsed.schema.resolve(field->typeId, "baudRate");
auto value = parsed.schema.view(field->typeId, responseBytes);
if (!handle || !value)
    return;
auto baud = value.value.get<std::uint32_t>(handle.value);
if (!baud)
    return;
useBaudRate(baud.value);
```

The example assumes the descriptor describes that field/member as a `U32`.
Each lookup/read reports failure explicitly if the peer uses another schema.
The handle can be reused for subsequent payloads of the same root type in the
same schema generation. `member(name)`, `member(index)` and `index(element)`
return checked subtree views for exploratory access. `get<T>()` reads an exact
scalar leaf. `enumUnderlying<T>()` reads an enum's exact integer type;
`enumName()` returns `{known, name}` within a checked result.

Paths support nested members and array indexes, such as `motor.samples[2]`.
An empty path selects the root. `resolve(typeId, span<const PathStep>)` provides
explicit member/index steps when a member name contains path punctuation.
Catalog and endpoint enumeration includes Fields, Commands and Services; this
module supplies metadata and local value reads/edits, without dynamic request submission.

## Find an endpoint by name

`findField`, `findCommand` and `findService` accept either separate exact group
and endpoint names, or one path such as `"device.Config"`:

```cpp
const auto* config = parsed.schema.findField("device.Config");
const auto* reset = parsed.schema.findCommand("control", "Reset");
const auto* inspect = parsed.schema.findService("control.Inspect");
```

`findEndpoint(category, group, name)` and `findEndpoint(category, path)` provide
the same lookup when the category is selected at runtime. Names are
case-sensitive UTF-8 bytes; no wildcard, member navigation or request is implied.
The path is the literal group name, one dot, then the literal endpoint name.
Both names may contain dots. If `"a.b.c"` matches both group `"a"` / endpoint
`"b.c"` and group `"a.b"` / endpoint `"c"`, the path overload returns `nullptr`;
the separate-name overload addresses either endpoint unambiguously. Categories
are independent, so the same path can identify a Field and a Service.

Missing names, an invalid category and a stale schema return `nullptr`.
Lookups scan the published metadata without allocating or changing it. A returned
pointer borrows the descriptor/index storage, just like `endpoints()`; it must
not be retained across workspace reset, reparse or destruction. It is metadata,
not a session binding: remote access still uses DynamicClient's admission and
generation checks.

## Stage and commit edits

Include `<telemetry/runtime/DynamicValueWriter.hpp>`. `begin` returns a move-only
editor directly. It validates the entire original value and copies its `N` wire
bytes into the supplied scratch span. It constructs no native DTO, allocates no
hidden storage, and uses the schema by value rather than borrowing the local
`RuntimeSchema` or `ParseResult` variable.

```cpp
auto handle = parsed.schema.resolve(field->typeId, "baudRate");
if (!handle)
    return;
auto editor = rt::DynamicValueWriter::begin(
    parsed.schema, field->typeId, responseBytes, scratchBytes);
if (!editor) {
    report(editor.status());
    return;
}
const std::uint32_t baud = 115200;
if (editor.set(handle.value, baud) != rt::Status::Ok)
    return;
auto committed = editor.commit(outputBytes);
if (!committed)
    return;
useCompleteEncodedValue(outputBytes.first(committed.value));
```

The source and output each describe exactly `N` bytes. Scratch has at least `N`
bytes; its **entire supplied span** must be separate from the original value,
the output, descriptor bytes, schema/editor objects, workspace control object
and published index elements. This includes unused scratch tail bytes. Schema
overlap checks inspect the published rows, not unused capacity in caller arrays.
The output must also avoid the editor and live schema storage. The original
value may share read-only schema bytes during `begin`; this permits no write to
that schema storage. The output may alias the original value when its storage
meets the output rules, so a successful commit can replace an existing encoded
buffer in place. The editor needs the
original bytes only during `begin`; it subsequently edits the scratch copy.
Scratch remains alive, writable and exclusively owned until the editor is
discarded or commit finishes. Do not modify its contents behind the editor.

`set(handle, const T&)` requires the exact scalar wire type and a matching
workspace, generation and root. `setEnumUnderlying(handle, const T&)` similarly
requires an enum with that exact underlying integer type. Enums remain open:
an unnamed numeric code is valid, without inventing a dictionary label.
Floating-point setters copy the referenced value's representation and emit
little-endian bits without numeric conversion, preserving signed zero and NaN
payloads, including signaling NaNs. The caller must supply those bits intact;
prior arithmetic or a conversion may already have changed them.

Every editor failure is sticky. A rejected `set` leaves staging unchanged and
prevents later commit; a failed `commit` leaves its destination unchanged. A
successful commit consumes the editor. Move the editor when transferring its
ownership; copying is disabled. The workspace must remain alive, and a reset or
any reparse makes the old editor stale before it accesses metadata or staging.
Invalidate the generation before releasing borrowed backing storage.

The unchanged-buffer guarantee assumes the storage/lifetime rules above. In
particular, scratch and output cannot contain the editor's own control bytes:
reporting a sticky error updates that editor object. An overlap diagnostic does
not make such self-aliasing control storage a supported destination.

This is a local logical transaction. External synchronization must protect
concurrent readers, writers and the application's publication of the output.
Commit does not make a buffer update atomic for another CPU thread, an interrupt
or DMA, and it does not supply a device-side compare-and-swap operation.

The [offline writer example](../examples/runtime_schema/Writer.cpp) changes a
nested scalar and an open enum, validates the result, and writes an output file.
It does not send a device request.

## Read, edit, write and refresh

A GUI can discover a Writable field, read a complete value, edit scalar leaves,
commit the new encoded value, submit a write through its transport adapter,
then refresh the field after the checked write result. Local encoding success
does not establish endpoint permission, peer schema admission or execution.
Writing a full value from an older read may overwrite changes made elsewhere;
the runtime writer provides no concurrency/version comparison with the device.

A future dynamic binding can reuse the existing
[`submitEncoded` FIFO](AsyncClient.md), with no second execution queue. It must
bind each operation to the workspace identity, schema generation and admitted
transport session, rechecking them before handoff and before using completion
data. The binding also retains endpoint ID, operation and request/response type
IDs. Submission checks operation permission, exact wire sizes, types and schema
admission; a completion must still match the current schema and session before
its bytes can become a value view or GUI state. A new session or descriptor
invalidates earlier handles and pending cache
updates. It must respect cancellation, `OutcomeUnknown` and backend recovery
contracts. No `DynamicClient` is implemented here. Existing static Codec policies
and their schema admission rules remain unchanged.

## Storage and lifetime

The application owns these storage lifetimes:

| Storage | Requirement |
| --- | --- |
| Descriptor bytes | Remain alive and immutable while schema metadata or names are used |
| Workspace object | Remains alive at a stable address until all schemas, handles, views and editors are gone |
| Index arrays | Remain alive and immutable for their published generation; writable again before another parse |
| Payload bytes | Remain alive and immutable while a value view is used |
| Writer scratch | Remains alive, writable and exclusive while its editor can access staging |

Metadata names are `std::string_view` slices of the original descriptor.
Wire offsets exclude C++ object padding and native alignment. Scalar reads
permit unaligned payloads and decode canonical little-endian bits.

Resetting a workspace or attempting a new parse invalidates old schemas,
value views, prepared handles and editors. This generation check requires the workspace
object itself to remain alive; it cannot detect an object that was already
destroyed or storage that the application changed behind a borrowed span.
Invalidate views before replacing buffers, and do not access stale metadata
pointers. Parsing and reset require external synchronization; simultaneous
read-only access is allowed while all borrowed storage remains immutable.

Schemas, parse results, views and handles can be copied or moved. They retain
the workspace identity and generation by value; they do not borrow the original
schema or parse-result object. These operations neither transfer storage
ownership nor extend the lifetime of descriptor, workspace or payload buffers.
Editors also retain the schema binding by value, but are move-only because they
exclusively borrow mutable staging.

A prepared handle identifies its workspace, generation, root type, leaf type
and offset. Reading with a handle checks its binding and bounds without another
member-name lookup or structural traversal. A handle for one root cannot be
applied to a different root or workspace, even when their type IDs coincide.

## Validation and value semantics

Descriptor validation covers the fingerprint, strict header/version/flags,
section and record bounds, reserved bytes, exact record exhaustion, positional
IDs, canonical built-in types, backward type references, checked wire sizes,
nesting and expanded-node budgets, UTF-8 strings, unique names within their
scopes, sorted enum codes, catalog coverage and endpoint type/capability rules.
Workspace capacity does not make an unchecked descriptor trusted.

A value view requires the exact wire size and validates Boolean representations
once when the root view is created. Subtree navigation checks metadata and
bounds without rescanning the payload. Prepared scalar reads access only the
selected leaf after checking the schema generation and handle identity.
Scalars require their exact wire type; a request for
a different C++ scalar reports a type mismatch rather than a numeric conversion.
Floating-point values preserve signed zero, infinities and NaN bit patterns.
Enums remain open, matching the existing codec: unnamed underlying integer
codes are valid values and report an unknown label.

## Build

```sh
cmake -S . -B /tmp/telemetry-runtime -DTELEMETRY_COMPONENTS=runtime \
  -DCMAKE_INSTALL_PREFIX=/tmp/telemetry-runtime-install
cmake --install /tmp/telemetry-runtime
```

```cmake
find_package(Telemetry REQUIRED COMPONENTS runtime)
target_link_libraries(application PRIVATE Telemetry::runtime)
```

```qmake
include(path/to/telemetry/lib/telemetry/runtime/runtime.pri)
```

## Verification

[The runtime tests](../tests/runtime_schema/README.md) exercise a native Host
Model, its real descriptor and encoded response, the generated standalone C++
codec, runtime metadata, dynamic value reads and transactional writer checks. Their deterministic malformed
corpus is checked against the production Python descriptor parser.

```sh
python3 -B tests/runtime_schema/run.py --cxx g++ \
  --build-dir /tmp/runtime-schema-gcc
python3 -B tests/runtime_schema/run.py --cxx clang++-18 --sanitize \
  --build-dir /tmp/runtime-schema-sanitized
```

The installed-module suite checks relocation and negative includes for the
standalone runtime component and compiles/links it for Cortex-M7 without
executing a board image. These checks establish software behavior only.

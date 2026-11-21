# Telemetry (C++20)

Authors: Ruslan Kovtun (shpegun60), codexAi. Own code: [MIT](LICENSE).

`telemetry` defines native typed Fields, Commands and Services, their local
tables and global catalogs, and one mixed `Model`. The Model derives an
immutable type registry from all three families. The optional
[`resource::telemetry::v3` providers](../integration/telemetry/v3/README.md) expose
the canonical descriptor and values files; the [JavaScript codec](../../web/telemetry.js)
decodes those wire types. Applications own transport framing, connection
state, synchronization and request correlation.

For runtime native IDs use Command `callAs` and Service `callAs<Result>`.
Field `readBorrowed<T>` returns an exact const view; `readAsResult<T>` retains
failure reasons. Flat `forEachEntry` includes IDs and catalog names, while
typed/erased `While` traversal stops on false. See the
[practical guide](../../doc/user/Ergonomics.md) and
[complete example](../../examples/user_guide/Ergonomics.cpp).

Runtime Command `call(id[, request])` and Service `callAs<Response>` /
`callBorrowed<Response>` expose one outcome without nested access. See the
[flat native guide](../../doc/user/FlatNative.md) and
[runnable example](../../examples/user_guide/FlatNative.cpp). Existing detailed
`callAs<ServiceResult<Response>>` and static calls remain available.

## Reading the source

Each authored source file starts with its purpose and the reason its boundary
exists. Class and API comments state ownership, borrowed lifetimes, unchecked
preconditions and failure behavior. Classes with public methods include a compact
method list, with overloads grouped by purpose. Internal comments explain the invariants
that are easy to lose during maintenance, including one target selection per
call, reverse-order Workspace release and validation before owner mutation.

Start with [Field](field/Field.hpp), [Command](command/Command.hpp) or
[Service](service/Service.hpp) for a native declaration. Their local tables
keep the original definitions alongside erased runtime entries; catalog tables
borrow local tables and assign group positions. [Model](model/Model.hpp) then
combines the catalogs with one [type registry](type/Registry.hpp).

The [public codec](codec/Codec.hpp) owns standalone byte-boundary validation.
Encoded table entries establish their own bounds before using
[internal codec steps](detail/Encoded.hpp), so the same checks do not recur in
every typed thunk. [Binding](detail/Binding.hpp) centralizes target storage and
selection, while [result types](result) keep routing failures separate from
application outcomes. These layers share declarations without adding a runtime
mode to native calls.

## Native API

A Field getter returns an unqualified supported `T` by value or borrows `const T&`. Its optional
setter takes exactly `T` or `const T&` and returns `WriteResult`. A Command
takes no argument or one aggregate request and returns `CommandResult`.
A Service takes no argument or one aggregate request and returns an aggregate response,
`void`, `ServiceResult<T>`, `const T&`, or `BorrowedServiceResult<T>` for an existing
aggregate response with an application status.
Callbacks are `noexcept`.
Known functions and methods use `field<getter, setter>`, `command<target>`
and `service<target>`; parameter forms accept native function pointers,
capture-free lambdas, stable callable lvalues and slots.

This complete example uses all three families and the compiled encoded
adapter. The native and encoded calls share the same model definitions.

```cpp
#include <telemetry/Telemetry.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

struct Config { float target; std::uint16_t rpm; bool enabled; };
struct Request { std::uint16_t channel; };
struct Response { float target; std::uint16_t channel; };

struct Device {
    Config config{230.f, 1500, true};
    std::uint16_t readRpm() const noexcept { return config.rpm; }
    Config readConfig() const noexcept { return config; }
    telemetry::WriteResult writeConfig(const Config& value) noexcept {
        config = value;
        return telemetry::WriteResult::Applied;
    }
    telemetry::CommandResult configure(const Config& value) noexcept {
        config = value;
        return telemetry::CommandResult::Executed;
    }
    Response readCalibration(const Request& request) const noexcept {
        return {config.target + request.channel, request.channel};
    }
};

inline Device device;
inline constexpr telemetry::FieldTable localFields{
    telemetry::field<&Device::readRpm>("RPM", device),
    telemetry::field<&Device::readConfig, &Device::writeConfig>("Config", device)
};
inline constexpr telemetry::CommandTable localCommands{
    telemetry::command<&Device::configure>("Configure", device)
};
inline constexpr telemetry::ServiceTable localServices{
    telemetry::service<&Device::readCalibration>("Calibration", device)
};
inline constexpr telemetry::FieldCatalogTable fields{
    telemetry::group("motor", localFields)
};
inline constexpr telemetry::CommandCatalogTable commands{
    telemetry::group("motor", localCommands)
};
inline constexpr telemetry::ServiceCatalogTable services{
    telemetry::group("motor", localServices)
};
inline constexpr telemetry::Model model{fields, commands, services};

int main() {
    constexpr auto rpmId = telemetry::makeId<0, 0>();
    constexpr auto configId = telemetry::makeId<0, 1>();
    constexpr auto callId = telemetry::makeId<0, 0>();
    const auto rpm = localFields.read<0>(); // optional<uint16_t>
    const auto config = fields.read<configId>(); // optional<Config>
    if (!rpm || *rpm != 1500 || !config || !config->enabled) return 1;
    if (localFields.write<1>(Config{240.f, 1600, true}) !=
        telemetry::WriteResult::Applied) return 2;
    if (commands.call<callId>(Config{250.f, 1800, true}) !=
        telemetry::CommandResult::Executed) return 3;
    const auto converted = fields.readAs<double>(rpmId);
    if (!converted || *converted != 1800.0) return 4;
    const auto native = services.call<callId>(Request{7});
    if (!native.hasValue() || native.value().target != 257.f) return 5;

    std::array<std::byte, telemetry::wireSize<Request>> input{};
    std::array<std::byte, telemetry::wireSize<Response>> output{};
    std::array<std::byte, model.maxScratch()> scratch{};
    telemetry::Workspace workspace{scratch};
    if (telemetry::encode(Request{7}, input) != telemetry::CodecStatus::Ok)
        return 6;
    const auto encoded = telemetry::callServiceEncoded(
        model.view(), callId, input, output, workspace);
    return encoded.dispatch == telemetry::DispatchStatus::Ok &&
           encoded.endpointStatus == telemetry::ServiceStatus::Ok &&
           encoded.written == output.size() ? 0 : 7;
}
```

Exact native `read/write/call` retain the original C++ types. `readAs/writeAs`
explicitly add checked native numeric conversion; aggregates and arrays still
require the exact type. Floating-to-integer conversion truncates toward zero
with checked bounds. Integer conversions check bounds without first passing
through floating point. Floating targets may round and accept NaN/Inf, but
finite narrowing overflow fails. Bool accepts finite zero/nonzero; enums use
their underlying integer representation. Failed conversion does not invoke a
setter. A runtime structural mismatch does not invoke a getter.

Local positions may be integers or enums. A global `PackedId` is a `u32`
group/position pair made by `makeId<Group, Position>()`; Field, Command and
Service IDs occupy separate spaces. By-value Field reads return an owning
`optional<T>`. `readAs<Big>` likewise owns `Big`, so a large native return may
use a large stack frame according to the compiler's return ABI.
`ServiceResult<T>` owns its optional response; use `successFrom(factory)` for
large responses. `success(value)` is restricted to objects of at most 256 bytes.

### Reading existing objects by const reference

A getter or Service that returns exactly `const T&` selects a borrowed result:

```cpp
struct CachedDevice {
    Config config{230.f, 1500, true};
    const Config& readConfig() const noexcept { return config; }
};
inline CachedDevice cached;
inline constexpr telemetry::FieldTable cachedFields{
    telemetry::field<&CachedDevice::readConfig>("Config", cached)
};
inline constexpr telemetry::ServiceTable cachedServices{
    telemetry::service<&CachedDevice::readConfig>("Config", cached)
};

// Both results refer directly to cached.config. Neither contains a Config.
auto view = cachedFields.read<0>();     // BorrowedValue<Config>
auto reply = cachedServices.call<0>();  // BorrowedServiceResult<Config>
if (view && reply.hasValue()) {
    auto target = view->target;
    const Config& same = reply.value();
    (void)target;
    (void)same;
}
auto snapshot = cachedFields.readAs<Config, 0>(); // owning optional<Config>
```

`BorrowedValue<T>` contains one pointer; an empty target produces an empty view.
`BorrowedServiceResult<T>` contains status and a view; an empty target returns
`ServiceStatus::Unavailable`. Access is const through `value()`, `*`, `->` or
`valueOrNull()`. Mutable/volatile references, rvalue references and pointer
returns remain unsupported. A Service response still must be an aggregate;
Field values may also be scalars, enums and arrays. The usual setter accepts
canonical `T` or `const T&`, independently of the getter's ownership mode.
For a fallible borrowed Service, return `BorrowedServiceResult<T>` by value:
`success(existingObject)` or `failure(ServiceStatus::Busy)`. Failed responses
encode no payload. Result wrappers are unwrapped only for Services and never
become descriptor types; Field getters return only `T` or `const T&`.

The application keeps the referenced object alive and stable for the entire
encoding operation, and for every later use of a native result. A view does
not extend lifetime or capture a snapshot. Returning a local object or a
by-value callback parameter cannot satisfy that contract. A native response
that refers into a caller's Request cannot outlive that Request. Slot reset or
rebind does not keep the previously referenced object alive.

Encoded borrowed reads/responses serialize directly from the referenced object,
without a native payload copy or a response Workspace lease. They reject overlap
between the complete native object (including padding) and the output prefix
they write. This check occurs after the callback supplies the address, so it
does not undo callback side effects. TypeIds, descriptor/fingerprint and wire
bytes depend on canonical `T`, not ownership. See the
[borrowed value contract](../../doc/BorrowedNativeValues.md) for storage and qualification details.

`get<I>()` returns a const definition reference; `forEach` traverses exact
definitions in declaration order, and `visit(position, visitor)` selects one
definition at runtime. Global forms use a packed ID and preserve group names.
Traversal does not call a getter or construct a live value. Runtime visitors
use indexed dispatch specialized for each visitor type; their code size is a
separate cost from a known-target native call. See the
[API tests](../../tests/structured/README.md) for the checked contracts.

### Group trees, paths and named handles

A device with many subsystems can keep each one in a branch file and mount
the branches: `telemetry::group(name, items...)` mixes subgroups, Fields,
Commands and Services, and `Model{adc::tlm::branch, metrology::tlm::branch}`
composes them (descriptor v3.1). Endpoints are then reached by path,
`group/.../endpoint` followed by `.member` and `[i]`, resolved at compile time:

```cpp
inline constexpr telemetry::Model model{adc::tlm::branch, metrology::tlm::branch};
inline constexpr auto gain1 = telemetry::endpoint<model, "adc/channel1/Gain">;
inline constexpr auto rate = telemetry::endpoint<model, "adc/Config.rateHz">; // reads only
inline constexpr auto reset = telemetry::endpoint<model, "adc/Reset">;

gain1.read();                                   // optional<float>
gain1.write(2.5f);                              // WriteResult
reset.call();                                   // CommandResult
telemetry::read<model, "adc/Status.raw[1]">();  // one-off access without a handle
```

A handle is an empty object whose type carries the PackedId and the value
type; the path is checked once, where the handle is declared, and a call
compiles to the same code as typed access by a `makeId` ID. A member handle
reads only. `makeId` and the three-catalog `Model{fields, commands, services}`
are unchanged. See [Model](../../doc/user/Model.md#group-tree-модель-із-файлів-гілок)
and the [group tree example](../../examples/group_tree/README.md).

### Type names

Every enum and struct in a descriptor carries a type name taken from the code:
the innermost scope and the type's own name (`adc::Config`). A
`reflection::TypeReflection<T>` specialization replaces it with
`typeName("...")` or removes it with `noTypeName()`, and must precede the
first descriptor containing `T`, like `EnumReflection`. The project switch
`TELEMETRY_TYPE_NAMES` (default 1, in [Config.hpp](Config.hpp), set in the
project's own `telemetry_config.hpp`) at 0 leaves every name out and the
descriptor of a flat Model is byte for byte v3.0, which hosts built before descriptor v3.1
need. See [Fields](../../doc/user/Fields.md#8-імена-типів-у-descriptor).

## Encoded access and storage

The canonical codec writes exact `wireSize<T>` bytes: little-endian scalar
bytes, enum underlying codes, arrays in element order and aggregates in member
order. It writes no C++ padding, tags or per-value lengths. Unknown representable
enum codes are valid. Bool bytes must be 0 or 1. Use ordinary aligned aggregates;
compiler-specific packed objects are outside the supported contract.

The Model exposes `fieldIndex`, `commandIndex` and `serviceIndex` for checked
runtime encoded routing. The compiled adapter also provides `readFieldEncoded`,
`writeFieldEncoded`, `executeCommandEncoded` and `callServiceEncoded`. These
routes check IDs, exact request length, full successful response capacity,
required scratch and encoded bool representations before invoking application
code. Encoded result `dispatch` describes routing/buffer/payload failures;
read `endpointStatus` only when `dispatch == Ok`. Application value validation
belongs to the callback. `Accepted` reports the callback's status without
creating a queue or retaining its request.

`TELEMETRY_STRUCTURED_LOCAL_BYTES` defaults to **32 bytes** (in
[Config.hpp](Config.hpp), with every project switch; a project sets it in its
own `telemetry_config.hpp` or with `-D`) and controls only
encoded payload storage. An owning Field value or Command request that fits uses a
local object; larger objects use caller-owned `Workspace`. For a Service, the
Request consumes this budget first, then `ServiceResult<Response>` fits only
if its complete wrapper fits the remainder. A borrowed Service budgets only
its Request: its status/view is control data, and its existing Response is
never copied into local or Workspace storage. A borrowed Field read needs no
payload scratch; a writable definition still advertises its setter's decode
requirement separately as `writeScratchBytes`. Values uses only `readScratchBytes`.
Zero selects Workspace for all owning/decoded payload objects. This policy is selected at compile time and does not change
native returns, supported types or wire bytes. It is a budget for live payload
object sizes, not a bound on total stack use or callback stack use.

`model.maxScratch()` is a sufficient bound for one encoded operation, including
alignment. Nested or concurrent operations need space for all live leases or
separate Workspaces. Scratch leases must be released in reverse order. A fully
local endpoint does not access Workspace. If scratch is required, wire bytes
must be disjoint from the complete Workspace span, including active outer
leases. Service input and output may overlap each other: decoding finishes
before a response is written. Standalone `encode/decode` require their wire
span to be disjoint from the native object. Successful or rejected operations
release the leases they acquire; Workspace and its storage remain caller-owned.

## Borrowed lifetimes and slots

Definitions borrow owners, callable lvalues, slots and immutable names. These
objects and any transitive references must outlive their definitions and every
table/catalog/model view that uses them. Tables own definitions and runtime
entries with internal references, and cannot be copied or moved. Catalogs
borrow stable tables and cannot be copied or moved. The Model borrows catalogs;
descriptors and resource providers borrow the Model's metadata.

The retained [slot families](slot/README.md) support late binding of owners,
functions, context functions and delegates. A call snapshots its slot once.
An empty slot yields an unavailable result without invoking application code;
an absent Field setter capability yields `ReadOnly`. Direct owner bindings
require a valid owner and have no null-owner branch. Slot rebinding and calls
require external synchronization. The library does not supply cross-field
snapshot coherence; application getters decide which state they publish.

## Integration, reflection and ABI

Include `<telemetry/Telemetry.hpp>` for the complete API or the required
family headers. A manual C++20 build needs include directories `lib`,
`lib/boost_pfr/include` and `lib/magic_enum`. Only
[`reflection/detail`](reflection/detail) includes the pinned Boost.PFR and
magic_enum backends. Public `reflection` facades supply aggregate, callable
and enum facts; explicit enum dictionaries can replace automatic names.
`TypeRegistry` deduplicates exact C++ types and records structural metadata;
it carries no live owner addresses, values, units, limits or defaults.

For qmake:

```qmake
include(path/to/lib/telemetry/telemetry.pri)
# Optional descriptor.bin / values.bin providers:
CONFIG += resource_telemetry
include(path/to/lib/integration/integration.pri)
```

`telemetry.pri` selects C++20 after including its dependencies and adds
`abi/StructuredAbi.cpp` and `model/Adapter.cpp` once. A manual build using the
compiled adapter links those two objects. Header-only native calls and direct
index calls do not require either object. Integration adds its compiled values
adapter through `integration.pri`, including the selected adapter's Telemetry
and Resource dependencies. Generic resources do not select it implicitly.

Modules exchanging in-memory views can call
[`requireStructuredAbi()`](abi/StructuredAbi.hpp) at an explicit boundary and
link `StructuredAbi.cpp`. The compiled Model and values adapters carry the
exact layout tag automatically. Define the storage macro identically in all
translation units: its value participates in that tag. These checks detect
revision, size and offset mismatches at link time; header-only boundaries need
their own explicit ABI dependency. Equal layouts with changed semantics require
an explicit `structuredAbiRevision` change. None of these boundary checks runs
during a native typed value call.

ABI revision **6** splits Field read/write scratch requirements. In the
qualified Cortex-M7 layout, FieldEntry is 32 bytes/alignment 4; CommandEntry
and ServiceEntry remain 20/4 and 24/4. This in-memory change does not alter
wire v3.0 or its existing descriptor/values fixtures.

The [frozen contract](../../doc/StructuredTelemetryV3FreezeQualification.md)
records wire constants and dependency pins. The
[protocol](../../examples/structured_protocol/README.md) and
[client](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/structured_client/README.md) examples keep application
transport behavior outside the library.

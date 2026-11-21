# Type names, group trees and endpoint handles in generated SDKs

[Documentation](README.md) · [Code generator](CodeGenerator.md) ·
[Generated C++](GeneratedCppApi.md) · [Generated JS/TS](GeneratedTsJsApi.md) ·
[Wire v3.1](WireV3.md) · [DynamicClient](DynamicClient.md) · [Dynamic JS](DynamicJsClient.md)

Clients address endpoints by path and through handles: `device.read<"adc/Config">(cb)`
and `device.adc.Config` in C++, `await device.read('adc/Config')` in TS/JS, and
`client.endpoint("adc/Config")` in the Dynamic clients
([handles](#endpoint-handles-of-a-bound-device),
[the same operation in four places](user/APIOverview.md#одна-операція-в-чотирьох-місцях)).
The per-family symbols (`fields::`, `commands::`, `services::`) still generate
but are [being removed](user/APIOverview.md#виводиться-з-ужитку).

Descriptor v3.1 adds three things to a device's descriptor: a unique type name on
an enum or struct (`adc::Config`), a group tree (groups inside groups, each holding
Fields, Commands and Services side by side) and a skippable attribute area. The
generator reads all three. Everything here is an **addition**: every symbol that a
v3.0 descriptor generates keeps its name and signature, and a v3.0 input gives
byte-identical output (pinned by `tests/ux_codegen_extension/legacy_output_hashes.json`
and `tests/codegen/test_names.py`).

## Type names

A named type is defined under its own symbol in `detail::<scope>`, and `Type_N`
stays an alias of it, so `detail::Type_N` (used by the UX QML extension and by
existing code) keeps compiling. The public spelling follows the wire scope:

| Wire name | C++ | TS/JS |
| --- | --- | --- |
| `adc::Config` | `device_api::types::adc::Config` | `types.adc.Config` (type and codec) |
| `Mode` (no scope) | `device_api::types::Mode` | `types.Mode` (type and enum dictionary) |

```cpp
device_api::types::adc::Config config{};          // the same type as detail::Type_12
static_assert(std::is_same_v<device_api::types::adc::Config, device_api::detail::Type_12>);
```

```ts
import {types} from './ts/device.js';
const config: types.adc.Config = {gain: 1.5, rateHz: 1000, enabled: true};
const bytes = types.adc.Config.encode(config);     // the codec of the named type
```

Symbols come from the generator's allocator in a dedicated scope: a struct called
`Status` stays `Status` there; keywords, macros, `Type_N`, `std`, `codecs`, `enums`
and the JS object hazards (`then`, `toJSON`, `prototype`) get a suffix. Structural
metadata (`--structural-metadata`) gains `name`, `cppName` and `jsName` on named
types and a `path` on every endpoint, only for a v3.1 descriptor.

`TelemetryBindings.hpp` gives every enum and struct its exact wire name, or
`noTypeName()`, for a v3.1 descriptor, so a native Model built from the generated
DTOs re-emits the input descriptor byte for byte (tested in `tests/codegen`). For a
v3.0 descriptor the file is unchanged; such a Model is built with
`withoutTypeNames(model)` to keep v3.0 bytes.

## Group trees

A group becomes a C++ namespace or a JS/TS frozen object, and each endpoint a
constant in it, beside the group's subgroups:

```cpp
using namespace device_api;
codec::encode(adc::channel1::Gain, 2.5f, out);       // the path adc/channel1/Gain
static_assert(adc::Config.id == adc::fields::Config.id);
```

```ts
import {adc} from './ts/device.js';
await client.write(adc.channel1.Gain, 2.5);          // adc.channel1.Gain === adc.channel1.fields.Gain
```

The flat form's `fields::`, `commands::`, `services::` and per-group `types::`
constants are generated in every group as well (plan section 15.5), so code written
for a flat model compiles unchanged after the device moves to the group form with the
same group names (`tests/codegen`, `names-aliases-*`). These aliases are
[being removed](user/APIOverview.md#виводиться-з-ужитку) with the per-family symbols. Names in one group share one
scope; a support name such as `Status` gets the same `_value` suffix it gets in the
flat `fields::` namespace (`adc::Status_value`). Every JS/TS endpoint object of a tree
carries its `path`.

## Attributes

The IR keeps every attribute area raw, as (record, tag, bytes); host outputs (types,
codecs, metadata) never read it, so an attributed descriptor generates the same host
code as its stripped form apart from the descriptor identity literals.
`TelemetryBindings.hpp` is refused for it with an `#error` naming every tag, because a
native Model built from the DTOs cannot re-emit attributes yet; `RpcBindings.hpp`,
which includes it, is refused with it.

`inspect` lists every endpoint in the section-15 path grammar, the type names and
the attributes:

```text
"adc/channel1/Gain -> Field, 0x00010000, f32",
"adc/Calibrate -> Service, 0x00000000, (adc::Reference) -> adc::Offset",
{"record": "type 15", "tag": "0x40", "class": "application", "value": "617070"}
```

## Endpoint handles of a bound device

For a v3.1 descriptor the generated client also binds every endpoint to a client as
a **handle**. A handle keeps its client and endpoint (its PackedId and value type),
is cheap to copy and stays valid as long as the client does. The nested form, the
path form and, in C++, the free function give the same handle. A group tree nests
groups (`device.adc.channel1.Gain`); a flat model keeps its families
(`device.motor.fields.Config`, path `motor/Config`). A flat path that names
endpoints of two families has no path form, as `RuntimeSchema::findEndpoint`
refuses it too; a root group named like a device method gets a suffix (`read_2`).

### C++ (`ClientBindings.hpp`)

```cpp
#include <device_api/ClientBindings.hpp>
using Client = device_api::client::DeviceClient<>;
Client client{backend, clock};
device_api::client::Device<Client> device{client};

auto gain1 = device.adc.channel1.Gain;                         // a handle kept in a variable
auto same = device.endpoint<"adc/channel1/Gain">();             // the same handle
auto also = device_api::client::endpoint<"adc/channel1/Gain">(device);
gain1.read([](const telemetry::client::Result<float>& r) noexcept { /* r.value() */ });
gain1.write(2.5f);                                              // or write(value, callback)
device.read<"adc/Config">(callback);
device.write<"adc/Config">(config, callback);
device.call<"adc/Reset">();                                     // a Command without a request
device.call<"adc/Calibrate">(device_api::types::adc::Reference{2.0f}, callback);
```

Results arrive through the client's callbacks, as for `client.read(endpoint,
callback)`; `write(value)` and `call()` without a callback ignore the result. The
path resolves at compile time against a generated table used only in constant
evaluation, so no path text reaches the image (`tests/client_handles` scans one built
without debug information, which spells template arguments). A
wrong path or value type is a build error whose instantiation note shows the path,
for example `P = Path<18>{"adc/channel9/Gain"}` with "No endpoint at path P in this
device"; `read<P>()` of a Command and `call<P>()` of a Field are refused the same way.
Member paths (`adc/Config.gain`) are the Dynamic client's: the generated device has
no member path form.

### TS and JS (`handles.ts`, `handles.js`, with `--client-runtime`)

```ts
import {bindDevice} from './ts/handles.js';
const device = bindDevice(client);                 // the bundled DeviceClient
const gain1 = device.adc.channel1.Gain;            // the same object as device.endpoint('adc/channel1/Gain')
await gain1.write(2.5);
const config = (await device.read('adc/Config')).value;   // types.adc.Config | undefined
await device.write('adc/Config', {...config!, rateHz: 2000});
await device.call('adc/Reset');
const offset = (await device.call('adc/Calibrate', {volts: 2})).value;
```

TypeScript checks a path against the string-literal union `Path` (with `FieldPath`,
`WritablePath` and `CallPath`), infers the value type through `ValueAt<P>` and
refuses a read-only write or a wrong kind at compile time. JavaScript checks at run
time and throws the Dynamic client's wording.

## The Dynamic clients: `endpoint(path)`

`DynamicClient::endpoint(path)` (C++) and `DynamicClient.endpoint(path)` (JS) resolve a
path once against the admitted descriptor and return a handle with `read()`,
`write(value)` and `call(request)`, using the runtime value types (a `DynamicValueView`
and canonical payload bytes in C++, plain values in JS). A member path
(`adc/Config.rateHz`, `adc/Status.raw[1]`) is readable: the whole Field is read and
the member delivered. After another admission a kept handle refuses with
`NotAdmitted`.

Every client uses the same wording, naming the path (JS throws it; C++ returns a
`PathStatus`, from `endpoint()` or `handle.accepts(operation)`, and `describePath()`
writes the text):

| Refusal | Wording |
| --- | --- |
| no endpoint | `No endpoint at path "adc/channel9/Gain"` |
| no member | `No member at path "adc/Config.nope"` |
| wrong kind | `"adc/Reset" is a Command: read() takes a Field`; `"adc/Config" is a Field: call() takes a Command or Service` |
| read-only | `"adc/Status" is a read-only Field` |
| member write | `"adc/Config.rateHz" is a member: write() takes a whole Field` |
| member of a call | `"adc/Calibrate.volts" names a member of a Service: only a Field's members are readable` |
| no admission | `No admitted descriptor to resolve path "adc/Config"` |

`tests/client_handles` runs the same paths through the generated device and the
Dynamic client, in C++ and in JS, against one native Model: equal values, equal
refusals, and the nested, path and free-function forms give the same handle.

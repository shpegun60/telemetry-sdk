# Generated TypeScript and JavaScript

The JS target is a browser-compatible ES module plus `.d.ts`; the TS target is
a strict TypeScript source module. They expose the same runtime contract.
Neither imports the Descriptor parser or walks a runtime type graph.
For a v3.1 descriptor, `types.adc.Config`, nested group objects and the
`bindDevice(client)` handles (`device.endpoint('adc/channel1/Gain')`) are
described in [type names, group trees and endpoint handles](GeneratedNamesAndHandles.md).

```ts
import {device, codecs, fingerprint} from './generated/js/device.js';
const request: device.types.InspectRequest = {config, multiplier: 4};
const payload = device.services.Inspect.encode(request);
// Custom transport sends operation, endpoint.id and payload.
// Check transport, dispatch, endpoint status and exact length before decoding:
const response = device.services.Inspect.decodeResponse(responsePayload);
```

Catalog exports have `types`, `fields`, `commands`, `services`. Fields expose
`read` and, when writable, `write`; Command/Service exports carry their own
`operation`. The four Operation string constants are `FieldRead`, `FieldWrite`,
`Command`, `Service`. Type IDs, packed IDs and capability bits retain their
descriptor meanings. Exported contracts and enum label maps are frozen.

`codecs.Type_<id>` contains `wireBytes`, `encode`, `encodeInto`, `decode` for
each reachable canonical type. `encode` allocates a Uint8Array;
`encodeInto(value, callerBuffer)` reuses an exact-size caller buffer. Decode
returns owned arrays/objects. Byte inputs/outputs support ArrayBuffer, typed
views and DataView with correct byte offsets. DTO arrays are ordinary arrays
of the exact descriptor length. Decoded structs have null prototypes and
preserve original member names, including literal `__proto__` and Unicode.
Normalized exported symbols are recorded in the generated manifest.

## Numeric semantics

| Wire type | JS/TS value |
| --- | --- |
| Bool | `boolean`; wire accepts only 0 and 1 |
| U8/I8/U16/I16/U32/I32 | integral finite `number` in exact range |
| U64/I64 | `bigint` in exact range |
| Enum | its underlying number/bigint, including unknown representable values |
| F32/F64 | `number`; NaN, infinity and negative zero are supported |

A finite number that rounds to F32 infinity is refused. F32 rounds through
the wire's IEEE representation. JavaScript Number does not promise preservation
of every NaN payload/signalling bit; tests compare the documented NaN semantic
exception, while ordinary values and signed zero have exact canonical bytes.
There are no physical-unit ranges, defaults, min/max/step assumptions or
application validation inferred from a wire type.

Invalid values, shapes, lengths and bool encodings throw TelemetryCodecError.
`encodeInto` may write an earlier member's prefix before a later member is
rejected; use a separate staging buffer if publication must be atomic. Each
struct member/array element is read once while its value is validated/written.
Caller-owned payload storage does not make the JavaScript engine allocation-free:
shape validation and decoded aggregates can allocate ordinary JS objects.
Codec errors do not represent transport or endpoint errors. In particular a
failed zero-byte Service must never be accepted by simply decoding its empty
payload. A pure codec can decode bytes from any source; the operation adapter
owns the separate status gate.

The [web example](../examples/codegen_web/README.md) has a bounded optional
Resource/RPC adapter accepting an injected complete-packet exchange. It is
example transport code, not part of the generated codec or a new required
protocol. It preserves Command Accepted separately from physical completion.
For your own non-RPC protocol, use endpoint operation/ID plus encoded payload
and its explicit response status contract. See
[schema admission](GeneratedSchemaCompatibility.md).

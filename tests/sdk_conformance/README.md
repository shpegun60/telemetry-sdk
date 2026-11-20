# Four-surface SDK conformance

This host suite runs **C++ static, C++ dynamic, JS static and JS dynamic** through the same
real native Model and Encoded dispatcher. It reuses `../batch_static/Model.hpp` and
`Descriptor.cpp`; the persistent JS peer is `../client_dynamic_web/Bridge.cpp`.
There is no second protocol engine, Model or response mock.

The C++ consumers build against a **relocated CMake installation**. The runner rejects
source-tree SDK include paths in every consumer compilation. Both the checked-in JS
runtime/generated JS SDK and a fresh compilation of the TypeScript sources run the
same four-surface comparison. `Consumer.ts` compiles those public declarations in strict
mode, including generated DTOs and inferred ReadMany/Batch tuples, without type assertions.

## Public usage

After opening one session and discovering capabilities, a static TypeScript consumer uses
normal generated endpoints. Transport/discovery happens once, outside these calls:

```ts
const draft: control.types.Config = {
    number: 37, enabled: true, gains: [0, 65535, 17], mode: -123,
    large: 0xffffffffffffffffn, negative: -0x8000000000000000n, f32: 4.25, f64: -0
};
await client.write(control.fields.Config, draft);
const reply = await client.service(control.services.Echo, draft);
const batch = await client.batch(tbm1, Batch.items(
    Batch.write(control.fields.Config, draft), Batch.read(control.fields.Config),
    Batch.command(control.commands.Accepted, draft),
    Batch.service(control.services.Echo, draft)));
if (batch.items?.[1].valueStatus === 'Ok')
    console.log(batch.items[1].value.number);
```

`Consumer.ts` also shows the other operations and runtime `unknown` narrowing. The
executed public calls are in `NativeConformance.cpp` and `Conformance.mjs`; no happy-path
application code manually encodes bytes. The C++ runtime fixture prepares canonical DTOs
with `DynamicValueWriter`, independently of the generated static Codec. Its zero seed is
validated for this fixture and is **not** a device-default guarantee.

## Exact scenario and oracle

Session 17, group 1, deliberately overlapping Field/Command/Service numeric IDs:

1. Write Config with number 1111; the native setter normalizes it to 1000.
2. Read Config; verify 1000 and all other member values.
3. Configure Command with number 37; native state becomes `37 ^ 0x55 == 112`.
4. Echo Service with number 99.
5. ReadMany Counter and Bool; verify 112 and true.
6. Mixed Batch: write Config 888, read the same Field, Accepted Command, Busy Service,
   and successful Echo Service 55. Verify order and distinct per-item outcomes.
7. Repeat that Batch as a separate stale postlude: dispatch executes, then admission is
   revoked after handoff and before receiving the reply. Known outcomes survive and all
   typed values disappear.

All four surfaces must produce byte-identical complete TEQ1 request/reply frames, checked
operation facts, value presence, final native counters and the exact independently stated
16-event trace. Request magic, session and monotonically assigned IDs are checked separately.
Data includes exact U64 max/S64 min, open signed enum -123, array values, finite F32 4.25
and bit-exact negative-zero F64. This suite does not claim JS Number preserves arbitrary
NaN payloads; raw-byte NaN preservation has separate SDK tests.

## Existing API distinctions are preserved

The report records actual `valueStatus` labels and asserts their expected values. Its
comparison removes only that label, then compares checked outcomes and value presence:

- Ordinary C++/JS static results do not expose a separate value-status label.
- Static JS ReadMany returns its checked tuple in `result.value`; runtime JS uses
  `result.items`. The report calls both collections `items`.
- C++ Batch results expose an operation object per item; JS items expose those facts
  directly. The report records the same fields without changing their values.
- After revocation C++ static/dynamic and JS dynamic label every Batch item `Stale`.
  JS static labels mutations/failed Services `NoValue` and value-bearing items `Stale`.
  Both retain Accepted versus Executed and suppress every typed value.
- The report's aggregate `hasValue` means at least one typed item value, rather than the
  existence of a tuple/container. This is a test observation, not a new public API.
- Generated/runtime JS DTOs may use a null prototype. DTO checks compare their exact own
  member keys and values, not Object.prototype identity.

These are conformance observations, not a claim that all language APIs have identical
object shapes or failure vocabulary.

## Optional structural metadata

Generation explicitly requests `--structural-metadata`. JSON, generated JS metadata and
freshly compiled TS metadata must match; every module object/array is frozen. The full
wire type graph, enum code decimal strings, member offsets, catalog ranges and categorized
endpoints are independently reconstructed from the existing runtime Descriptor parser;
for a v3.1 descriptor so are the type names and endpoint paths (Dynamic/Static parity).
C++ versus JS member spelling is checked explicitly (`number_value` versus `number`).

`../sdk_metadata` separately checks edge-case metadata, default-output compatibility,
standalone standard-library-only C++ metadata and the offline ARM root. This suite does
not duplicate those probes or require UI hints, defaults or MCU-side metadata code.

## Run

```sh
python3 -B tests/sdk_conformance/run.py --build-dir /tmp/telemetry-sdk-conformance \
  --node /path/to/node --tsc /path/to/typescript/lib/tsc.js --cxx g++
python3 -B tests/sdk_conformance/run.py --build-dir /tmp/telemetry-sdk-conformance-asan \
  --node /path/to/node --tsc /path/to/typescript/lib/tsc.js --cxx clang++-18 --sanitize
```

Use a fresh external directory. `--cmake` and `--ninja` accept explicit tool paths. The
runner retains commands/logs, executed binary hashes, installed/generated file hashes,
both normalized observations and source hashes before/after. Minimum counted results:
321 native checks plus 287 for each JS/TS four-surface comparison (895 per host profile).
No hardware is connected, reset or programmed; no ARM execution or total-stack/RAM claim
is made by this host suite.

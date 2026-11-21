# Headless structural JS payload checks and Qt Core example

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

[web/telemetry.js](../../../web/telemetry.js) is the final v3 payload codec.
It requires BigInt/DataView support; CI uses Node 22. It imports no transport,
resource protocol or Bind/Exchange. The former v2 decoder is retired.

The maintained HTML editor, HTTP example and browser suite are now owned by
[Telemetry UX payload tools](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/structured_client/README.md) and its
[installed-package tests](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/tests/structured_client/README.md). This SDK runner remains headless;
`--browser` fails with an actionable migration diagnostic before any output
directory or compiler/Node process is created. The retained Qt example is a
[Qt Core console smoke](../../../examples/structured_client/README.md).

## Client API

```js
import * as ts from './telemetry.js';

const model = ts.parseDescriptor(descriptorBytes);
const service = model.services[0];
const payload = ts.encodeServiceRequest(model, service.id, requestObject);
// An application adapter transmits payload and handles statuses/correlation.
const response = ts.decodeServiceResponse(model, service.id, responseBytes);
const values = ts.decodeValues(model, valuesBytes);
```

Public structural rows use `typeId` for Fields and `requestTypeId` plus
`responseTypeId` for Services. Commands have `requestTypeId`. Type rows expose
scalar codes, enum entries, struct members and array element/count metadata.
`encodeValue/decodeValue` operate directly on any validated registry TypeId;
`encodeFieldWrite/decodeFieldValue/encodeCommandRequest` provide named routing.
Unknown packed IDs are rejected at u32 width. Void is JavaScript `undefined`.

Parsed models/rows/arrays are immutable; lookup Maps and the model identity
check are private. A cloned/hand-written object is not a validated model.
DescriptorCache validates each newly supplied byte sequence, returns the
cached model for identical bytes and refuses equal-hash/different-byte input.
It has bounded capacity and no session state. Fingerprint comparison for
values uses the cached descriptor fingerprint; live values are never hashed.

Scalar representations are bool, Number for integers up to 32 bits/F32/F64,
and BigInt for U64/S64. Integer range/fractional errors are rejected; F32 is
rounded to binary32, with finite overflow rejected. NaN/Inf and negative zero
are supported. A decoded JS NaN does not promise its original payload, sign or
signaling/quiet bits: a JS engine may quiet or canonicalize it. Retain the raw
bytes for an exact bit-pattern roundtrip, or declare raw bits as U32/U64 payload
in the application model. See the [public numerical/raw-bit contract](../../../doc/user/DescriptorAndValues.md#javascript-number-та-точні-біти).
Enum dictionaries label values without restricting representable unknown codes. Structs decode
to objects with a null prototype; encoding requires all and only their named
members. Arrays require exactly the declared length.

Descriptor parsing verifies v3.0, all reserved bytes and record versions,
section ordering/count arithmetic, positional IDs, backward non-Void type
references, wire sizes, request/response shape and strict UTF-8/NUL rules.
Resource ceilings match the default C++ profile and may be tightened by
`parseDescriptor(bytes, { typeDepth: 16, ... })`. Counts/lengths are checked
before record-list/string allocation; input size is checked before copying.
Values checks header identity, exact dense token length/status, unavailable
zero payloads and canonical bool representations.

## Reproduce

```sh
python3 tests/structured/client/run.py --cxx g++ --node node --build-dir build/client
python3 tests/structured/client/run.py --cxx g++ --null-checks --build-dir build/client-null
python3 tests/structured/client/run.py --cxx clang++-18 --sanitize --build-dir build/client-san
```

Generated fixtures and logs remain in the chosen build directory. Node is a
desktop test dependency, not a dependency of the MCU library. Playwright is
required only by the separate UX browser suite. The headless runner also
executes a negative CLI control with nonexistent compiler/Node names: the
retired browser option must return exit 2 and leave its output nonexistent.

## Current headless gate and retained earlier evidence

The 8892-check headless gate remains active. Browser/Qt results below retain
their recorded earlier scope; they do not imply that this SDK runner still
ships or executes Browser.py. Current browser acceptance is in the linked UX suite.

The runner requires the exact `Check.mjs` report: 8892 integer checks (8889 before the three group-tree ceilings), zero
integer failures and `cppInterop: true`, with exactly those three fields.
One positive and sixteen result mutation controls reject duplicate/missing
fields, extra fields, wrong types, changed counts and failure statuses. These
parser controls are separate from the interoperability tally below.

- `Check.mjs`: **8892 counted checks**, including every truncated prefix of
  three independently frozen descriptors, semantic mutations with recomputed
  fingerprints, resource/depth/expansion bounds, literal/prototype names,
  scalar extrema, unknown enums, arrays/structs/Void and values errors.
  A valid model at the default descriptor ceiling reaches group/entry 65535
  and packed ID `0xffffffff` without signed JS bitwise narrowing.
  The 2026-10-11 additions contribute **5832 checks**: 128 assertions over 32
  F32/F64 patterns, 5556 recomputed-fingerprint descriptor mutations, 146 Values
  byte mutations and two corpus outcome controls. Each byte is substituted with
  XOR masks `1` and `128` in the three small frozen descriptor fixtures and the
  73-byte Values fixture. Descriptor fingerprint bytes are excluded because
  rehashing replaces them. Mutations may remain valid; invalid input must raise
  `TelemetryFormatError`, and accepted output must retain validated identity,
  immutable descriptor shape or positional Values rows. Both acceptance and
  rejection are required in each corpus. This is a finite deterministic corpus,
  not a claim of exhaustive malformed-input coverage.
  Floating checks preserve finite/zero/infinity bits, require only numerical
  NaN semantics, and prove exact U32/U64 relay for every tested representation.
- `Device.cpp`: C++ generates descriptor, values and native request/response
  bytes. JS reads them and sends its own payloads through real encoded Field,
  Command and Service indexes. All application statuses, unavailable target,
  exact callback counts and wrong payload length are checked. It uses the
  example's existing library ABI/Workspace, not a second JS-aware endpoint.
- `Resources.cpp`: the client also consumes the unchanged Stage 10 fixture.
  Both newly emitted C++ descriptors pass the independent Python oracle.
- `Browser.py`: **30 counted checks** in a real browser, including a recursive
  form, exact downloaded bytes, U64/S64/unknown enum display, every application
  status, Void/unavailable, fingerprint mismatch, literal HTML-like text, and
  a delayed operation that executes once despite the client timeout. Deferred
  real File reads cover service selection ABA, identical-descriptor reload,
  stale errors and values, and fresh native response/values decoding afterward.
- `QtSmoke.cpp`: Qt 6.10.1 MinGW C++20 smoke passed with `-Werror`, exact 41-byte
  response, U64/S64 preserved and native readAs using the same model.

The final-path preview on 2026-10-03 passed the same 3057 JS checks and 20
Chromium browser checks, plus the Qt smoke, against the migrated C++ API.

The current host runner executes five successful interoperability commands;
its browser-migration refusal control is reported separately. Historical
runs with the former browser option executed six successful commands;
those are command counts, separate from the assertions above. Local MinGW
13.1, GCC 13.3 with null checks and Clang 18 ASan/UBSan runs passed. The original Stage 12 browser run used Edge through
Playwright 1.63.0; the final-path preview and CI use pinned Chromium. The
original UI rendering was inspected
from `client-desktop.png`. The test-only optional protocol remains covered by
its existing correlation/wrap/disconnect suite, not made mandatory here.

This is desktop correctness/integration evidence, not new MCU timing.
Stage 12 originally changed no library C++/qmake input, wire format or endpoint
ABI. The final migration uses `<telemetry/Telemetry.hpp>`, namespace
`telemetry`, and `integration/telemetry/v3` providers; it keeps the same native
DTOs and canonical bytes. Whole-program stack and MCU cycle evidence is
retained separately in [Stage 14 qualification](../mcu/h7s/README.md).

On 2026-10-11 the expanded 8889-check suite passed with portable official
Node 22.23.3 on MinGW GCC 13.1, the same GCC with null-pointer checks retained,
and Linux Clang 18 with ASan/UBSan. Each run completed five commands, validated
two generated descriptors independently and passed all sixteen report-parser
mutation controls. Production `web/telemetry.js` remained unchanged. Browser
checks were not rerun for this test/documentation addition; the 20 browser
checks above describe their recorded earlier run.

## Final-source hardware evidence, 2026-10-03

The [current MCU results](../mcu/h7s/RESULTS.md),
[descriptor run](../descriptor/h7s/README.md) and
[values-provider run](../resources/h7s/README.md) record the final native API
and v3 provider paths at source
`01fd180ff3678d7c47066f8a728dc1e02f0f3973`, with `source_dirty=true` retained
and captured input/image hashes. Both O2/Os profiles completed and full
internal Flash restoration was verified. This verifies the MCU producers;
it is separate from the browser/Qt interoperability checks above and does
not measure browser rendering or network throughput.

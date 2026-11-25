# JS/TS ReadMany qualification

This suite qualifies the optional Encoded operation 5 through the existing web
`DeviceClient` FIFO. It does not add RPC batching, a second client engine, runtime
Descriptor admission, or a GUI. All compiler outputs and reports go outside the
repository. It never accesses hardware.

## Public use

Generate the normal JS/TS SDK with `--client-runtime`. The generated scalar and
open-enum read bindings include `readManyScalar`; Struct, Array and legacy
bindings without that explicit marker remain ineligible, even when their wire
size is four bytes. Handwritten scalar bindings can provide the same explicit
underlying `ScalarCode` marker.

```ts
const backend = new EncodedBackend(sendFrame, {readMany: true});
const client = new DeviceClient({backend, matchesHeader: sdk.matchesHeader});
// Transport integration forwards complete received frames to backend.receive().
if (!client.openSession(session, descriptorHeader)) return;

// TRM1 is discovered by the application's existing Resource integration.
const bound = client.bindReadMany(trm1, [sdk.device.Voltage,
                                       sdk.device.Counter]);
if (bound.status !== Status.Ok) return;
const result = await client.readMany(bound.binding);
if (!result.succeeded || !result.value) return;
const [voltage, counter] = result.value;
if (voltage.status === ReadManyStatus.Ok) console.log(voltage.value);
if (counter.status === ReadManyStatus.Unavailable) console.log('Counter unavailable');
```

The tuple preserves input order and generated value types, including `bigint`
for U64/S64. Cache the opaque binding for its admitted client/session. TRM1,
source arrays, metadata and request bytes are copied or captured at bind time;
mutating the originals cannot alter a queued operation. A binding from another
client or an earlier session is refused. Local transport opt-in and matching
remote capability are independently required. RPC returns `Unsupported` before
any Resource WRITE; no individual-read fallback is hidden in this method.

Results own their response and item bytes; no borrowed RX spans escape. Objects
and tuple entries are frozen; their owned `Uint8Array` contents are caller-owned
and can be changed without altering a sibling item or the outer payload. JS
Promises, arrays and byte copies allocate normally; no C++ no-heap claim applies.
No simultaneous-sampling or exactly-once execution guarantee is added. An
explicitly chosen retrying backend retains its documented repeated execution
policy and immutable request identity.

The entire bounded reply and canonical scalar bytes are checked before the first
user decoder runs. A valid TEQ1 envelope with invalid DTO bytes or a throwing
decoder returns `CodecError`, retains checked dispatch/status/payload, and exposes
no partial tuple. Unlike C++ dynamic ReadMany's separate value-status field, this
uses the existing JS result shape. Synchronously closing/resetting in a ReadMany
decoder likewise preserves checked operation bytes while retiring values.
Operations 1-4 retain their wire/status contracts. Shared clock-reentry guards
also refuse a submission that changes admission inside `Clock.now()` and revoke
handoff when the clock callback closes or cancels the active operation.

Malformed TEQ1 envelopes, response-budget echoes or actual-length bounds retain
the transport's `MalformedReply`/recovery path. After authorized handoff, timeout
or cancellation of a ReadMany still requires physical/session recovery but does
not become `OutcomeUnknown`, because the operation is read-only. The exact budget
echo is retained even when successful per-item refusals shorten the actual body.

## Maintained checks

Run from the repository root with the existing Node and TypeScript toolchain:

```sh
python3 tests/client_read_many_web/run.py --build-dir /external/read-many-web \
  --node /absolute/node --tsc /absolute/typescript/bin/tsc --cxx g++
```

For the native interoperability peer under Clang ASan/UBSan, use `--cxx
clang++-18 --sanitize`. The runner checks byte-for-byte reproduction of published
`Client.js`/`Client.d.ts`, then runs both published JS and emitted TypeScript.
Strict consumers check inferred ordered tuples, status narrowing, U64 precision,
and rejected aggregate/legacy/Command/forged bindings. A persistent real C++
Model/dispatcher covers 32 Fields, mixed catalogs, open enum, U64/S64, signed zero,
NaN raw bytes, unavailable owner slots, per-item refusals and ordered interleaving
with FieldWrite, FieldRead, Command and Service on one FIFO.

Fake clocks distinguish queued, offered and prepared deadlines, support changes,
QueueFull, immutable retries, physical send leases, late replies, close/reset
decoder reentry, and retained operation outcomes. The ordinary operation 1-4 and
RPC regression suite remains `tests/client_web/run.py` and must also pass.

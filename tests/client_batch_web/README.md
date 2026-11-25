# JS/TS Mixed Batch

This suite uses one `DeviceClient` FIFO and a persistent native C++ Encoded
dispatcher built from the real Model in `../batch_static/Model.hpp`. It generates
both JavaScript and TypeScript bindings from that Model's Descriptor; no copied
handwritten SDK or mocked happy-path server is used.

Once the application has established transport, admitted a session, and obtained
the matching TBM1 capability, normal use is one call:

```ts
const result = await client.batch(tbm1, Batch.items(
    Batch.write(control.Config, draft),
    Batch.read(control.Config),
    Batch.command(control.Configure, args),
    Batch.service(control.Echo, query)
));
if (result.items?.[1].valueStatus === BatchValueStatus.Ok)
    console.log(result.items[1].value);
```

`Consumer.ts` is the strict compile-time consumer, with no type assertions or
manual byte encoding. The runtime `Integration.mjs` performs the complete native
exchange and checks independently recorded getter/setter/command/service traces.
`Check.mjs` uses a controlled clock and explicit handoff states for deterministic
timeout, cancellation, recovery, reentry, malformed-reply and ownership checks.

The outer Result describes the Encoded batch operation. Item Results describe
each checked endpoint outcome. A bad later value or stale session suppresses
every typed value while retaining already checked mutation outcomes. A malformed
inner batch body has no per-item outcomes. `Accepted` does not mean `Executed`.
Normal Encoded transport does not retry an uncertain mutation; an explicitly
selected retrying backend may execute it again.
Outer `succeeded` does not imply that every item succeeded or that typed values
are available: always inspect the selected item's outcome and `valueStatus`.

U64/S64 values use BigInt. JavaScript Number does not promise preservation of
signaling NaN payload bits after decoding/re-encoding; the retained raw item
payload is checked separately against native F32/F64 bytes. Empty Struct and
Void are distinct even when both have zero wire bytes.

Run offline with installed Node and TypeScript paths:

```sh
python3 -B tests/client_batch_web/run.py --build-dir /external/batch-gcc \
  --cxx g++ --node /path/to/node --tsc /path/to/typescript/lib/tsc.js
python3 -B tests/client_batch_web/run.py --build-dir /external/batch-clang \
  --cxx clang++-18 --sanitize --node /path/to/node --tsc /path/to/typescript/lib/tsc.js
```

All build outputs and generated SDKs remain outside the repository. The receipt
records actual source and native executable hashes. No hardware, browser,
network service, package download, Qt or runtime Descriptor parser is required.

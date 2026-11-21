# Code generator qualification

From the repository root, with a C++20 compiler, Node 22 and TypeScript 5.9.3:

```sh
python3 tests/codegen/run.py --cxx g++ --node node --tsc /path/to/typescript/bin/tsc --arm-cxx arm-none-eabi-g++ --build-dir /tmp/telemetry-codegen-gcc
python3 tests/codegen/run.py --cxx clang++-18 --node node --tsc /path/to/typescript/bin/tsc --sanitize --build-dir /tmp/telemetry-codegen-sanitized
python3 -m unittest discover -s tests/codegen -p 'test_*.py' -v
```

Add `--browser-python /path/to/playwright-venv/bin/python` to execute both emitted
JS and TS-compiled ESM in actual Chromium. Install the pinned CI browser tooling
into a separate environment with `pip install playwright==1.63.0` followed by
`python -m playwright install --with-deps chromium`.

All builds, generated SDKs, logs and `summary.json` remain outside the repository.
The runner downloads no tools and flashes no hardware. Standalone C++ compilation
deliberately has no Telemetry/PFR/magic_enum include paths. A second translation
unit exercises header linking. The optional native path checks explicit sparse
and 64-bit enum dictionaries. Direct Encoded and existing RpcClient integration
execute against an actual native Model; the RPC path includes fragmentation,
retry identity, one-shot execution and session/schema admission.

The native producer exports real Descriptor and canonical Config/Request/
Response/all-scalar/empty payloads. Both emitted JS and TS-compiled JS execute
against these bytes, the independent existing browser codec, direct Encoded
dispatch and Resource/RpcMailbox. Hostile names, integer limits, bool validation,
float semantics, offset views, arrays, open enum values, immutable metadata,
failed empty responses and malformed protocol envelopes have explicit checks.
Parser and publication controls include mutations, corrupted fingerprints,
readonly `--check`, modified/unowned output refusal, link traversal and rollback.

Counted execution floors are enforced by the runner. Compilation results and
runtime counts are separate. Benchmark records compare the same generated DTO
through native/generated codecs and retain checksums. Optional ARM measurements
record sections, undefined symbols and compiler stack reports. These are
compiler/host observations, not measured hardware speed or whole-firmware RAM.
Existing frozen golden files and historical hardware receipts remain unchanged.

`WireEditing.mjs` additionally checks opt-in JS and compiled TS preserving codecs
against the actual native Descriptor. It compares exact F32/F64 NaN/signed-zero
bytes, nested edits, offset views, seed ownership, invalid originals and ordinary
finite writes. A deliberately corrupted preservation branch must fail the same
byte oracle. Default generated SDKs retain their prior output without that option.

Descriptor v3.1 ([names.py](names.py), [test_names.py](test_names.py)): goldens for a
named flat model and a two-branch tree ([goldens](goldens), refreshed only by
`python tests/codegen/names.py write-goldens`); v3.0 inputs, which get the same group
form and handles (a kind suffix where two families share a name), against the
frozen hashes of `../ux_codegen_extension`; named, tree and attributed fixtures through every web
check; a native Model rebuilt from the generated DTOs re-emits the named, tree,
unnamed-tree, v3.0 and native descriptors byte for byte; RuntimeSchema against
generated metadata and `typeName<T>()` for every name and path; code written for a
flat model compiles against the same model in the group form (C++ and TS); every
path form of the generated device is its nested handle; `TelemetryBindings.hpp` is
refused for an attributed descriptor, naming the tags.

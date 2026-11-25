# Optional UX generator extension

The SDK generator owns Descriptor validation, immutable schema construction,
symbol allocation, C++/JS/TS source generation, manifests and transactional
publication. The optional `telemetry-ux-codegen` distribution owns QML source
generation and the thin browser starter. Qt is needed to compile generated C++, not to run either Python
package. No UI implementation is bundled in the minimal core wheel.

The existing CLI spelling remains:

```sh
telemetry-codegen generate descriptor.bin --targets qml --output generated
```

Select `qml` explicitly. It also emits the same C++ files as the `cpp` target.
The default remains `cpp,ts,js`; those paths do not import the optional UX
package. Missing UX gives an installation hint before publication. An installed
extension with incompatible API, IR or SDK version fails explicitly and is not
replaced with a hidden fallback.

## Installed ownership

| Distribution | Owns | Dependencies |
| --- | --- | --- |
| `telemetry-codegen-core` | `tools.telemetry_codegen`, canonical optional client assets and `telemetry-codegen` console command | No mandatory third-party runtime |
| `telemetry-ux-codegen` | `telemetry_ux_codegen`, QML emitters and browser starter | Exact matching core version |
| `telemetry-codegen` | Full-install dependency metadata only | Exact matching core and UX versions |

The full distribution has no Python modules and no second console command.
This keeps the familiar full installation available without a dependency cycle
or two owners for the same installed files. See [package installation](SDKPackages.md).

Use a fresh virtual environment. If migrating an environment containing the
old monolithic `telemetry-codegen` wheel, uninstall it **before** installing the
new full package. It formerly owned paths that now belong to core; arbitrary
in-place pip installation order is not a supported ownership transfer.
Uninstalling UX from a correctly split installation leaves the core command and
headless generation available.

## Version 1 emitter seam

The seam is API version 1 over IR version 2. IR 2 (the one group form) gives
every endpoint one public symbol, its symbol in its group (`Schema.api_groups`,
`Schema.api_symbols`); the generated SDK has no `fields::`, `commands::` or
`services::` namespaces, so an extension written for IR 1 is refused by version
rather than generating references to symbols that no longer exist.

`tools.telemetry_codegen.emitter_api` exports `API_VERSION`, `IR_VERSION`,
`SDK_VERSION`, `Schema`, `Endpoint`, `GeneratedTarget`, `KEYWORDS`, `identifier`,
`cpp_string_literal` and `reachable_type_ids`. The extension imports this public
surface; it does not import private C++ generator functions.

`Schema` and `Endpoint` are the same frozen SDK objects used by built-in
emitters. Schema exposes namespace, Descriptor SHA-256, exact declared header,
fingerprint, positional types, categorized endpoints and allocated member/enum
symbols. Its recursively frozen `raw` mapping contains validated type records
and categorized catalog/endpoint records, header totals/offsets and traversal
budgets. Type records retain wire sizes, scalar codes, underlying types, enum
entries, members or array element/count, plus validated `nodes` and `depth`
budgets used by bounded QML conversion. References always use original TypeIds.
No native C++ offsets or device defaults are inferred.

`cpp_string_literal(text)` escapes every original UTF-8 byte.
`reachable_type_ids(schema)` returns sorted transitive endpoint-reachable
TypeIds including Void ID 0. `identifier` and `KEYWORDS` use the SDK's existing
deterministic naming rules.

The fixed optional module `telemetry_ux_codegen` declares matching API/IR/SDK
versions and provides `generate_qml(schema) -> GeneratedTarget`. Its result
contains a relative-path/source-text mapping and a JSON-compatible QML symbol
mapping. The CLI calls it once, applies its ordinary path/collision/text checks
and includes the symbol mapping under the existing manifest key `qml`.
Non-finite JSON values are rejected. All rendering and validation finish before
the SDK's guarded output publication; the extension never publishes files.

The explicit `--ux-starter web` option requires `--targets js` and
`--structural-metadata`. The CLI checks both prerequisites before importing UX,
then calls `generate_web_starter(schema) -> GeneratedTarget` with the same frozen
schema. It applies the same result validation and publication contract and stores
the starter's symbol mapping under `web_starter`. Selecting QML and the starter
together imports the extension once. Neither option changes default output.

The browser starter emits only `ui/DevicePage.js`, its TypeScript declaration and
a short usage guide. The entry imports the generated SDK/metadata and shared
`telemetry-ux/web` view, taking an existing admitted client from the host. It
creates no transport, client, FIFO or additional form implementation. Custom
application code stays outside generated output.

This small seam is for installed trusted generators, not executable code from
a Descriptor. It does not add an endpoint engine, transport or request queue.
SDK, emitter API, structural metadata and wire versions remain distinct.

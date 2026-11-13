# Repository guidance

This is the standalone `shpegun60/telemetry-sdk` repository, checked out inside
the power analyzer workspace. Its changes do not update analyzer firmware
automatically. The public API is one C++20 `telemetry` library; `ts` may be
an application alias. See [README.md](README.md), [API](lib/telemetry/README.md)
and [migration guide](doc/StructuredTelemetryV3MigrationGuide.md).

- `lib/telemetry`: native Fields/Commands/Services, reflection/type registry,
  canonical LE codec, bounded encoded Model dispatch and slots.
- `lib/resource`: independent flat filesystem and generic packet protocol;
  `resource_telemetry` selects Descriptor/Values; independent
  `resource_telemetry_rpc` selects RpcMailbox. Both require an explicit
  telemetry manifest include.
- `examples/structured_protocol`: optional transport-owned Bind/Exchange.
- `examples`: SDK usage and device-integration examples; the standalone desktop
  application lives in `telemetry-qt-client`. `web/telemetry.js` is the v3 JS codec.
- `lib/delegate`, `lib/boost_pfr`, `lib/magic_enum`: pinned vendor trees.
  Preserve original tracked bytes and licenses.

Generate objects/logs under an explicit ignored `build/...` directory.
On Windows put the selected Qt MinGW bin first in PATH; resolve installed
kit/compiler paths rather than treating a recorded version path as current.
Run from the repository root:

```sh
python tests/run_checks.py --build-dir build/checks
python tests/resources/run.py --build-dir build/resource-core
python tests/structured/endpoints/run.py --build-dir build/endpoints
python tests/structured/qualification/run.py --build-dir build/qualification
python tests/structured/client/run.py --node node --build-dir build/client
```

For sanitizers use Linux Clang18 `--cxx clang++-18 --sanitize`. For ARM use
the actual CubeIDE compiler with `--arm --cxx <arm-none-eabi-g++>`. ARM
compile/link/objdump/size and individual `.su` frames are offline evidence,
not MCU execution. The [test index](tests/README.md) and CI workflow define
the maintained host/sanitizer/null-check/ARM/Qt matrix. C++17, old Scalar,
old JSON/v2 providers and no_json qmake configurations are retired.

Negative tests must fail for their intended diagnostic. Keep nonempty header
coverage, exact runtime counts, codegen/literal/relocation comparisons,
wire goldens, retained-symbol controls and source/provenance checks.
Native known-target codegen and runtime cycles are different evidence.

For a hardware run follow [the H7S contract](tests/structured/mcu/h7s/README.md).
Only the root coordinator accesses the explicitly selected adapter/COM port.
Build and bound every image before access; back up/restore the complete
internal Flash and verify a fresh readback. No implicit device selection.
Preserve historical receipts without changing their measured source identity.
Current receipts must match their captured inputs; CI success must refer to
the exact published SHA.

Names, callable/owner lvalues, slots and views borrow external lifetimes;
nonmoving tables and LIFO Workspace leases retain their documented contracts.
Application synchronization, validation and transport state remain external.
Endpoint declarations have only name+binding; no units/limits/defaults/flags
or parameter/member metadata are added to the structural type model.

Retired review transcripts, Scalar probes and superseded implementation plans
are archived outside this checkout; current contracts are in `doc/Architecture.md`
and `doc/WireV3.md`. See `doc/RepositoryMaintenance.md` for the verified manifest.
The exploratory `tests/review-2026-09-26` tree was archived externally on
2026-10-04 after full inventory and per-file hash verification; see
`doc/RepositoryMaintenance.md`. Preserve that archive and original reports;
do not recreate the retired probes as active tests without a current contract.
Never clear ignored build/evidence directories merely because they are ignored.

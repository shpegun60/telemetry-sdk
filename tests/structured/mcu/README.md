# MCU qualification: host, offline ARM and separate H7S execution

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

`run.py` executes the actual Mixed/Scale probe bodies on a host and compiles,
links and inspects Cortex-M7 images. It has no device, serial or programmer
integration. The separate [H7S runner](h7s/README.md) establishes actual
cycles and observed whole-probe-chain stack in its [results](h7s/RESULTS.md).

```sh
python tests/structured/mcu/run.py --cxx g++ --build-dir build/mcu-host
python tests/structured/mcu/run.py --cxx g++ --null-checks --build-dir build/mcu-null
python tests/structured/mcu/run.py --cxx clang++-18 --sanitize --build-dir build/mcu-san
python tests/structured/mcu/run.py --arm --cxx arm-none-eabi-g++ --build-dir build/mcu-arm
python tests/structured/mcu/receipt.py verify tests/structured/mcu/local-receipt.json --self-test
```

The default matrix is O2/Os/Og. Every runner records compiler/flags, exact
command results, LF-normalized inputs and compiler stack frames; changing
an input during the run refuses the evidence. ARM adds real linked symbols,
sections and disassembly and executes zero conditions.

The fixture keeps descriptor v3.0 while the SDK default emits type names:
`run.py` and the H7S image runner put [config](config/telemetry_config.hpp) on
the include path, whose `TELEMETRY_TYPE_NAMES 0` preserves the 1775-byte
descriptor and the condition counts below that the historical receipts record.

## Retained scenarios

- Mixed executes exactly 12,230 conditions per optimization and reports the
  unchanged 97-check multi-TU consumer separately. It covers exact Config
  Fields/Services, direct/local/global access, bound slots, visitors, readAs,
  encoded Field/Command/Service, arrays/structs, 4 KiB request/response paths,
  insufficient Workspace refusal, every descriptor cursor and Values.
- Scale executes exactly 2,316 conditions per optimization: 128 distinct
  targets and their direct indexed, direct known, local/global, named/lambda
  visitor, readAs and encoded paths under same/sequential/shuffled IDs.
  Invalid catalog/entry/max-u32 IDs refuse before callbacks. Only the old
  local/Scalar controls were retired after their baseline was retained.
- Every large Service response byte is compared, including in-place wire
  input/output. Workspace leases release scratch on both success and refusal.
- Local/global release instructions must match direct C++ or contain a
  resolved compiler tail forward to that same body. Raw and effective
  comparisons remain separate; a forward is an extra branch. Two positive
  and five changed-code/target controls exercise the parser.
- Null-check mode can retain target-address tests; its difference from
  direct C++ is recorded. Local/global must still agree. Og streams are
  recorded without imposing release instruction identity.

## Final-tree offline evidence

The [local receipt](local-receipt.json) records seven compiler/mode roles,
140 shared LF inputs and 42 configurations for the borrowed extension.
The captured HEAD is `595a65b`, with `source_dirty=true`: source/input hashes
match the later sealed code commit `7b73fb4`, rather than that earlier HEAD.
It is not a clean-tree claim. All three host roles executed 43,638 conditions
apiece (130,914 total); four ARM roles executed zero C++ conditions. Across
all roles, 424 compiler/link/inspection commands completed successfully.
Raw reports remain under `build/borrowed/mcu-<role>-canonical`; their digests
identify retained files, without recreating missing artifacts. The original
[Stage 20 receipt](../../../doc/evidence/pre-borrowed/offline/local-receipt.json)
retains its original capture and remains separate.

Each final library frame is checked, including files under `lib/telemetry`.
There is no directory-based legacy exemption. Individual frames are bounded
at 256 B, except multi-TU consumer roots with 512 B release/768 B Og bounds.
The real linked-symbol control must retain and refuse `_malloc_r` and
`_printf_r`; all final images also reject formatting, allocation, old Scalar
and startup metadata constructors. Freestanding failed contracts use a trap.

Actual CubeIDE14 individual root frames:

| Probe | O2, B | Os, B | Og, B |
| --- | ---: | ---: | ---: |
| 4 KiB encoded Field | 192 | 168 | 168 |
| 4 KiB encoded Command | 176 | 152 | 152 |
| 4 KiB encoded Service | 216 | 192 | 192 |

These are individual compiler frames, not full call-chain bounds. An owning
native Big return deliberately places a large result on the application
stack; the H7S probe measures its complete chain separately. Equal instruction
bytes do not prove equal cycles at different function addresses.

The pre-unification [offline/hardware proof](../../../doc/evidence/pre-unification/README.md)
retains its original counts, old Scalar comparison and source identity.
The final CI must qualify the published SHA independently of these local runs.

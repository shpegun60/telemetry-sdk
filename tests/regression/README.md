# Shared contract checks

These maintained fixtures cover the low-level IDs, number conversion and stable
slots used by the final C++20 telemetry core. They link no legacy serializer or
compiled adapter. The host and ARM runners use an explicit fixture manifest;
adding a source file alone does not add a gate.

[checks.py](checks.py) preserves original negative-case numbers where the
underlying shared contract is unchanged:

| Fixture | Maintained cases |
| --- | --- |
| OwnerSlotCompileFail | 1–3, 10–18 |
| FunctionSlotCompileFail | 1–5, 11–14, 21 |
| LateBoundCompileFail | 1–16, 23–42; heap-fallback controls 12/13 |
| SlotCallableCompileFail | 0 control, 1–15 refusals |
| BorrowedBraceCompileFail | 0 stable-object/target-view control, 43–56 refusals |
| OwnerLifetimeCompileFail | 14 |
| ReviewCompileFail | 12–15, 19–25 |
| IdBoundaryCompileFail | 0 control, 1–104 using the final typed/encoded APIs |
| ExplicitIdTemplateArgCompileFail | 1–9 |
| NullChecksMatrix | 1–6, 8, 10–13 valid; 20/23/25/26 null-target refusals |
| WeakTargetInstantiation | 0–6, 11 |

Removed cases tested old Scalar, Getter/Setter, metadata/limits/arguments,
serialized JSON/v2, old ABI layout, or old factory overloads. An old factory's
rejection of `std::ref` does not describe the final native Binding contract;
the retained low-level slot owner checks remain separate.

The numeric oracle reports actual tested input counts and skips explicitly when
long-double precision is insufficient. Hand-derived [NumericEdges](NumericEdges.cpp)
remain independent of that oracle. FP mode refusals include Clang's no-honor
flags as well as fast-math and finite-only modes.

[SlotCodegen](SlotCodegen.cpp) compares native slot-backed Field reads with
explicit selected-target calls. Function/context/borrowed/owned routes require
identical normalized instructions. Owner routing requires no added instruction
count and the same direct method relocation; register allocation and optional
empty-result paths can differ. Only trailing alignment nops after an
unconditional terminal transfer are omitted, and explicit branch targets keep
those nops in the comparison. Local branch targets match the function symbol,
offset and actual instruction address, then compare by instruction ordinal.
Relocations at call sites take precedence over local-looking address placeholders;
external call symbols and relocation targets remain checked. Reachable nops and
literals remain checked. [ArmSlotGateCheck.py](ArmSlotGateCheck.py) replays existing
O2/Os assembly logs and exercises padding, local-target and reference controls.
These are offline compiler comparisons, not MCU cycle measurements.

[DescriptorSizeProbe](DescriptorSizeProbe.cpp) serves a descriptor from one
linked root, `descriptor_probe`, so `run_arm_checks.py` can report what
serving it costs on Cortex-M7: the NUCLEO-H7S3L8 demo's flat model and the
group-tree fixture as a tree and as flat catalogs, each packed (consteval
bytes) and streaming (`DescriptorView::read`), with `TELEMETRY_TYPE_NAMES` 1
and 0. The probe checks at compile time that names cost exactly their records
(a u32 length and the bytes per name); the runner requires every type name in
each named image and none in a stripped one. See the
[size report](../README.md#arm-descriptor-size-report).

Runtime conditions and commands are counted by the executed fixtures and runner.
Per-command arguments, exit status and output are retained in the build logs.
No historical fixed pass total is used as the final suite's result.

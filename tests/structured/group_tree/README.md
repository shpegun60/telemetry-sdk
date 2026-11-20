# Group tree: a Model from branch files

This suite qualifies the descriptor v3.1 GroupTree mode and compile-time path
access: a Model mounted from two branch files that know nothing of each other,
[AdcBranch.hpp](AdcBranch.hpp) and [MetrologyBranch.hpp](MetrologyBranch.hpp),
each with its own `Config`. The format is in
[WireV3](../../../doc/WireV3.md#group-tree), the device author's side in
[Model](../../../doc/user/Model.md#group-tree-a-model-from-branch-files).

## Reproduce

```sh
python3 tests/structured/group_tree/run.py --cxx g++ --build-dir /external/group-tree
python3 tests/structured/group_tree/run.py --cxx clang++-18 --sanitize --build-dir /external/group-tree-sanitized
python3 tests/structured/group_tree/run.py --cxx g++ --cross clang=clang++-18 \
  --arm-cxx arm-none-eabi-g++ --build-dir /external/group-tree-matrix
```

On Windows a `--cross` compiler may be reached through WSL with a `wsl:`
prefix, as in the [type-names suite](../type_names/README.md). The build
directory must be outside the checkout. Nothing touches a device.

## What it checks

1. **Golden.** [golden.py](../descriptor/golden.py) builds `tree.hex` from
   explicit records: groups adc, adc/channel1, adc/channel2 and metrology in
   pre-order, two Gains, two Configs (`adc::Config`, `metrology::Config`),
   Status, Reset and Calibrate. The shared decoder table
   ([v31_cases.json](../descriptor/v31_cases.json)) carries the tree's accepted
   cases and one malformed descriptor per tree rule; the
   [type-names suite](../type_names/README.md) runs it in C++, JS and Python,
   with group, path and lookup lines in each model dump.
2. **Producer.** `Producer.cpp` builds `Model{adc::tlm::branch,
   metrology::tlm::branch}` with every given compiler, writes the descriptor
   and must equal `tree.hex`; with `TELEMETRY_TYPE_NAMES=0`, the same tree
   without type names. It checks the outline (depths, parents), one PackedId
   space shared by the families (`idOf<>` and `model.idOf<>()`), O(1) encoded
   dispatch through the unchanged `ModelView` with an empty row where a group
   has none of a family, the registry's two `Config` names, a flat Model over
   the same tables, ABI revision 6, and typed access by path: `read<>` of a
   Field, a member and an array element, `write<>` of a whole Field in each
   branch, `call<>` of a Command and a Service.
3. **Refusals.** `Negative.cpp`, one case at a time: a group or endpoint name
   with a path character, a Field and a Command of one name, a subgroup and a
   Field of one name, two root branches of one name, nine levels against the
   default eight, a 65-byte group name, a 304-byte path, a 65-byte member name
   in a tree, a 65-byte type-name part (flat too), and path access to no group,
   no endpoint, a group, the wrong kind of endpoint, no member, a member of a
   non-struct, an element past an array, a member write, a Field of another
   type, an unknown path in `model.idOf<>()`, a Command path to `read<>` and an
   empty segment. Five controls build: eight levels, a deeper or wider
   profile, the flat form with path characters and 65-byte names, and one
   endpoint name in two groups.
4. **ARM.** With `--arm-cxx`, the tree descriptor from ARM GCC must equal
   `tree.hex`, and `DispatchSize.cpp` links `dispatch_root` alone over the same
   endpoints as a tree and as today's flat catalogs.

`summary.json` records the producers, which diagnostics show the offending
path or name, and the ARM sizes.

## Measured on 20 November 2026

`tree.hex` (1130 B) is produced byte for byte by MinGW g++ 15.1.0, g++ 13.3.0
and clang++ 18.1.3 (WSL), and by arm-none-eabi-g++ 14.3.1 (CubeIDE 2.0.0). The
same endpoints as flat v3.0 catalogs take 1305 B: one group record replaces
up to three family catalog records.

What a diagnostic names, besides its message:

| Error | GCC 15.1 | GCC 13.3 | Clang 18.1.3 |
| --- | --- | --- | --- |
| path access (`read<model, "adc/channel3/Gain">`) | the path, `EndpointPath<18>{"..."}` | the path | the path |
| a path character in a group name | the name, in the `group(Name("adc/x"), ...)` expansion | the source line only | the name |
| a name or path over a ceiling | the descriptor's line, not the name | same | same |

Ceiling errors come from runtime-valued names checked in a constant
expression; C++20 shows the failing function, not its argument.

ARM encoded dispatch, `dispatch_root` linked alone with `--gc-sections`
(arm-none-eabi-g++ 14.3.1, Cortex-M7):

| | Flat catalogs | Group tree | Delta |
| --- | ---: | ---: | ---: |
| `.text` O2 | 648 B | 796 B | +148 B |
| `.rodata` O2 | 360 B | 640 B | +280 B |
| `.text` Os | 568 B | 704 B | +136 B |
| `.rodata` Os | 1678 B | 1886 B | +208 B |

The dispatch code is the same O(1) lookup; the flat build's Command and
Service catalogs hold one group, which GCC folds into constants, while the
tree's hold four rows, three of them empty (12 B each, plus 8 B per empty
type-catalog row). These are offline object sizes; no device was run.

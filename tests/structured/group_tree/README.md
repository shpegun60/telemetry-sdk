# Group tree: a Model from branch files

This suite qualifies the descriptor v3.1 GroupTree mode, compile-time path
access and named endpoint handles: a Model mounted from two branch files that know nothing of each other,
[AdcBranch.hpp](AdcBranch.hpp) and [MetrologyBranch.hpp](MetrologyBranch.hpp),
each with its own `Config`. The format is in
[WireV3](../../../doc/WireV3.md#group-tree), the device author's side in
[Model](../../../doc/user/Model.md#group-tree-модель-із-файлів-гілок).

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
   has none of a family, the registry's two `Config` names, the per-family
   catalog form of the same tables (`telemetry/testing/V30Model.hpp`, v3.0
   bytes), ABI revision 6, and typed access by path: `read<>` of a
   Field, a member and an array element, `write<>` of a whole Field in each
   branch, `call<>` of a Command and a Service. Named handles
   (`endpoint<model, "adc/channel1/Gain">`) for a Field, a member, an element,
   a struct Field of each branch, a Command and a Service: their PackedId,
   kind, value type and definition, an empty trivially copyable type that is
   the same for the same path, the same result types as the free functions,
   and reads, writes and calls (one through a stored copy) that agree with the
   path forms. `Values.cpp` serves the tree's live Values file
   (`resource::telemetry::v3::ValuesFile`, which counts a tree's Fields through
   the tables the tree builds): one token per Field in descriptor order, read
   whole and in every piece length up to 13, then back through the host's
   `ValuesReader` against the tree descriptor's `RuntimeSchema`, value for value
   with `read<model, path>()`, before and after a write by path.
3. **Refusals.** `Negative.cpp`, one case at a time: a group or endpoint name
   with a path character, a Field and a Command of one name, a subgroup and a
   Field of one name, two root branches of one name, nine levels against the
   default eight, a 65-byte group name, a 304-byte path, a 65-byte member name
   in a tree, a 65-byte type-name part (flat too), and path access to no group,
   no endpoint, a group, the wrong kind of endpoint, no member, a member of a
   non-struct, an element past an array, a member write, a Field of another
   type, an unknown path in `model.idOf<>()`, a Command path to `read<>` and an
   empty segment. A handle refuses at its declaration a path to no group, no
   endpoint, a group, no member, an element past an array, a member of a
   Command and an empty segment, and a flat model; at its use a write through a
   member handle, `read()` of a Command, `write()` of a Service, `call()` of a
   Field and a write of another type. Six controls build: eight levels, a
   deeper or wider profile, the flat form with path characters and 65-byte
   names, one endpoint name in two groups, and a handle used in constant
   expressions whose write of a read-only Field compiles (it returns
   `ReadOnly` at run time, as `fields().write<id>()` does).
4. **ARM.** With `--arm-cxx`, the tree descriptor from ARM GCC must equal
   `tree.hex`, and `DispatchSize.cpp` links `dispatch_root` alone over the same
   endpoints as a tree and as today's flat catalogs. `HandleSize.cpp` writes
   each operation three ways (a handle, the free path function, typed access by
   a `detail::makeId` ID) and is compiled with `-fno-ipa-icf`: every handle function
   must have exactly the instructions and relocations of the other two, at O2
   and Os, with a control that the comparison tells read from write. Its
   linked image must hold no path text.

`summary.json` records the producers, which diagnostics show the offending
path or name, the ARM sizes and the handle comparison.

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

A handle's path errors show the path the same way, at the handle's
declaration; an error at its use (a member write, a Command read) shows the
path only for a member handle, whose type carries it, and otherwise the
handle's name at the call.

ARM encoded dispatch, `dispatch_root` linked alone with `--gc-sections`
(arm-none-eabi-g++ 14.3.1, Cortex-M7):

| | Flat catalogs | Group tree | Delta |
| --- | ---: | ---: | ---: |
| `.text` O2 | 648 B | 796 B | +148 B |
| `.rodata` O2 | 360 B | 640 B | +280 B |
| `.text` Os | 568 B | 704 B | +136 B |
| `.rodata` Os | 1678 B | 1886 B | +208 B |

A named handle (arm-none-eabi-g++ 14.3.1, 20 November 2026) compiles to the
same instructions as the free function and as access by a `detail::makeId` ID
in every operation; function bytes:

| Operation | O2 handle / path / ID | Os handle / path / ID |
| --- | ---: | ---: |
| read a Field | 12 / 12 / 12 | 12 / 12 / 12 |
| read a member | 12 / 12 / 12 | 12 / 12 / 12 |
| write a Field | 20 / 20 / 20 | 20 / 20 / 20 |
| call a Command | 16 / 16 / 16 | 16 / 16 / 16 |
| call a Service | 20 / 20 / 20 | 40 / 40 / 40 |

The image linking all fifteen holds no path text.

The dispatch code is the same O(1) lookup; the flat build's Command and
Service catalogs hold one group, which GCC folds into constants, while the
tree's hold four rows, three of them empty (12 B each, plus 8 B per empty
type-catalog row). These are offline object sizes; no device was run.

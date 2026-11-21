# A device Model built from branch files

Author: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

A device with many subsystems keeps each one in its own branch file and
mounts the branches in one Model. The subsystem classes know nothing of
telemetry; each branch file is one expression with no tables and no IDs; one
top file composes the branches and declares the named handles the
application uses.

| File | What it holds |
| --- | --- |
| [adc/Adc.hpp](adc/Adc.hpp) | The ADC class: gains, configuration, status, reset, calibration. No SDK header |
| [adc/AdcTelemetry.hpp](adc/AdcTelemetry.hpp) | The ADC branch: `channel1` and `channel2` subgroups beside the `Config` and `Status` Fields, the `Reset` Command and the `Calibrate` Service |
| [metrology/Meter.hpp](metrology/Meter.hpp) | The metrology class, with its own `Config` and a `Spectrum<4>` template type |
| [metrology/MetrologyTelemetry.hpp](metrology/MetrologyTelemetry.hpp) | The metrology branch and the one `TypeReflection` this example writes |
| [Telemetry.hpp](Telemetry.hpp) | `Model{adc::tlm::branch, metrology::tlm::branch}`, its descriptor and the named handles |
| [main.cpp](main.cpp) | Handles, path access and the descriptor a host receives |
| [CMakeLists.txt](CMakeLists.txt) | A standalone executable; the SDK side is header-only here |

## Build and run

From the Telemetry root, with CMake, Ninja and a C++20 compiler:

```sh
cmake -S examples/group_tree -B /tmp/telemetry-group-tree -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/telemetry-group-tree
ctest --test-dir /tmp/telemetry-group-tree --output-on-failure
```

The program prints every endpoint of its own descriptor with its path and
type name, then `Group tree guide: 20 checks passed`:

```text
Field    adc/Config             adc::Config
Field    adc/Status             adc::Status
Field    adc/channel1/Gain
Field    adc/channel2/Gain
Field    metrology/Config       metrology::Config
Field    metrology/Spectrum     metrology::Spectrum4
Command  adc/Reset
Service  adc/Calibrate          adc::Reference -> adc::Offset
```

The [docs suite](../../tests/docs/README.md) builds and runs it on the host
and compiles and links it for Cortex-M7.

## The branch file

```cpp
inline constexpr auto branch =
    ts::group("adc", ts::group("channel1", ts::field<&gain<0>, &setGain<0>>("Gain")),
              ts::group("channel2", ts::field<&gain<1>, &setGain<1>>("Gain")),
              ts::field<&config, &setConfig>("Config"), ts::field<&status>("Status"),
              ts::command<&reset>("Reset"), ts::service<&calibrate>("Calibrate"));
```

`ts::group(name, items...)` takes subgroups, Fields, Commands and Services in
any mix. The small functions above it in the file adapt the plain class to
the signatures an endpoint takes (a setter returns `WriteResult`, a Command
`CommandResult`). Names in a tree are path segments: no `/`, `.`, `[`, `]` or
`\`, and unique among everything one group holds; either mistake is a build
error at the group that makes it.

## Type names

Every enum and struct the descriptor carries is named automatically with
its innermost namespace and its own name: `adc::Config` and
`metrology::Config` stay apart although both are `Config`. A template
specialization has no automatic name, so
[MetrologyTelemetry.hpp](metrology/MetrologyTelemetry.hpp) gives
`Spectrum<4>` one:

```cpp
template<>
struct telemetry::reflection::TypeReflection<metrology::Spectrum<4>> {
	static constexpr auto name = telemetry::reflection::typeName("metrology::Spectrum4");
};
```

`noTypeName()` in place of `typeName("...")` removes a name. A type's
`TypeReflection` lives in the file of the branch that owns the type, before
its group expression. `#define TELEMETRY_TYPE_NAMES 0` in the project's
`telemetry_config.hpp` leaves every name out; see
[Fields](../../doc/user/Fields.md#8-імена-типів-у-descriptor).

## Handles and paths

[Telemetry.hpp](Telemetry.hpp) declares one handle per endpoint the
application touches. The path is checked there, once: a path that names
nothing is a build error that shows it.

```cpp
inline constexpr auto gain1 = telemetry::endpoint<model, "adc/channel1/Gain">;
inline constexpr auto adcMode = telemetry::endpoint<model, "adc/Config.mode">; // reads only
inline constexpr auto calibrate = telemetry::endpoint<model, "adc/Calibrate">;

device::gain1.read();                                  // std::optional<float>
device::gain1.write(2.5f);                             // WriteResult
device::adcMode.read();                                // one member: the getter runs whole
device::calibrate.call(adc::Reference{1.25f});         // the Service's own argument
```

A handle is an empty object whose type carries the PackedId and the value
type: a call compiles to the same code as `telemetry::read<model, path>()`
or as typed access by ID, and no path text reaches the image (the
[group-tree suite](../../tests/structured/group_tree/README.md) compares the
ARM code). The free functions `read`, `write`, `call` and `idOf` with the
path as a template argument stay available for a one-off access. The full
grammar and every rule are in
[Model](../../doc/user/Model.md#group-tree-модель-із-файлів-гілок).

## The same paths in the clients

A host reaches these endpoints by the same paths: the generated Static C++
client (`device.read<"adc/Config">(cb)`, `device.adc.channel1.Gain`), TS/JS
(`await device.read('adc/Config')`) and the Dynamic client
(`client.endpoint("adc/Config.rateHz")`). The
[API map](../../doc/user/APIOverview.md#одна-операція-в-чотирьох-місцях)
shows one operation in all four places.

## The IDs a client shows

The groups are numbered in pre-order: `adc` 0, `adc/channel1` 1,
`adc/channel2` 2, `metrology` 3. A PackedId is the group and the endpoint's
position among that group's endpoints of its family, so `adc/channel1/Gain`
is `1:0`, `0x00010000`, as the client's Details shows it, and
`adc/Calibrate` is Service `0:0`. [main.cpp](main.cpp) checks both with
`device::gain1.id` and `telemetry::idOf<model, "adc/Calibrate">()`.
[Reading a Field without the SDK](../../doc/user/ReadingAFieldByHand.md)
reads `adc/channel1/Gain` from this Model over TCP byte by byte; an ID is
valid for its descriptor's fingerprint.

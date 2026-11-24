# Повні приклади посібника

Програми використовують поточний C++20 API без Qt, мережі та приладу. Вони
містять готові declarations і перевіряють результати викликів.

Почніть із [QuickStart.cpp](QuickStart.cpp): Field, Command і Service в одній
групі `ts::group("device", ...)`, named handles (`config.read()`,
`reset.call()`) і шлях члена `ts::read<model, "device/Config.voltage">()`.
Модель із кількох файлів гілок показує [group_tree](../group_tree/README.md).
Native, Encoded, Ergonomics і FlatNative теж будують `Model{...}` з груп:
типізований доступ іде через шляхи й handles, а runtime-ID виклики (`readAs`,
`visit`, `callAs`) — через `model.fields()`, `model.commands()` і
`model.services()` з ID від `idOf<model, "path">()` або `handle.id`.

| Програма | Що показує | Compiled sources |
| --- | --- | --- |
| [NativeOnly.cpp](NativeOnly.cpp) | Один local Field/Command/Service, static/runtime native calls, checked conversions і exact signatures; без Model/Workspace | Header-only; compiled sources відсутні |
| [QuickStart.cpp](QuickStart.cpp) | Мінімальна Model з однієї групи з головної сторінки: named handles і шлях члена | `StructuredAbi.cpp`, `Adapter.cpp` |
| [Native.cpp](Native.cpp) | Field/Command/Service; типи, всі slots, owning/borrowed outputs, typed та erased traversal | `StructuredAbi.cpp`, `Adapter.cpp` |
| [Resources.cpp](Resources.cpp) | Flat filesystem, lazy FileView, custom R/W provider, порційний LIST/STAT/READ/WRITE | `Protocol.cpp` |
| [Encoded.cpp](Encoded.cpp) | Власний complete-frame adapter без файлів, business validation, великі/borrowed responses | `StructuredAbi.cpp`, `Adapter.cpp` |
| [Ergonomics.cpp](Ergonomics.cpp) | Runtime native callAs, borrowed Field, причини відмов та flat/While traversal | `StructuredAbi.cpp` |
| [FlatNative.cpp](FlatNative.cpp) | Command call і Service callAs/callBorrowed з одним result/status; owning, borrowed, void | `StructuredAbi.cpp` |
| [ResourceClient.cpp](ResourceClient.cpp) | Checked resource request builders і borrowed reply parsers | `Protocol.cpp` |
| [Codec.cpp](Codec.cpp) | Nested DTO і `std::array`; existing object, RAII Workspace result та low-level Lease decode | Header-only; compiled sources відсутні |

```sh
python tests/docs/run.py --cxx g++ --build-dir /tmp/telemetry-doc-examples
```

Команда виконується з кореня репозиторію. Вкажіть встановлений compiler через
`--cxx`; на Windows це може бути повний шлях до Qt MinGW `g++.exe`.
Приклади не вимикають assertions. Для ARM `--arm` збирає їх без запуску на
приладі; це перевірка compiler/linker, а не апаратний вимір.

`Encoded.cpp` використовує простий **прикладний** envelope, щоб показати
зв'язок operation/ID/payload із library adapter. Цей envelope не оголошує
нового wire protocol бібліотеки. UART/TCP framing, таймаути, повтори й
синхронізація залишаються в транспорті застосунку.

Почніть із [API Map](../../doc/user/APIOverview.md), щоб обрати потрібну програму.
Читайте [Native API](../../doc/user/NativeApi.md) і
[Transport and Resources](../../doc/user/TransportAndResources.md) паралельно
з цими програмами. Команди й CI integration описані в
[documentation checks](../../tests/docs/README.md).
Standalone encoding/decoding без Model або endpoint declarations описано у
[Codec and Workspace](../../doc/user/CodecAndWorkspace.md).

Наступний крок після односторінкових програм —
[device integration](../device_integration/README.md). Він розносить business
class, metadata/facade та receive loop по різних файлах і показує приймання
split/coalesced frames для telemetry та files. Він теж перевіряється docs runner.

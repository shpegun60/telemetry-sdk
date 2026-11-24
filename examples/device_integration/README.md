# Приклад: бізнес-клас, фасад і приймання потоку

Це повний невеликий C++20 застосунок. Він показує, як підключити бібліотеку до
класу з реалізацією в `.cpp`, залишити таблиці всередині фасаду та передавати
йому завершені запити з UART або TCP. Програма працює на host: замість драйвера
`main.cpp` подає справжні порції байтів у bounded receiver і перевіряє відповіді.

| Файл | Відповідальність |
| --- | --- |
| [Device.hpp](Device.hpp), [Device.cpp](Device.cpp) | Два конфігураційні блоки, перевірка значень, зміна стану, лічильники викликів |
| [Api.hpp](Api.hpp) | Runtime API застосунку: native виклики, IDs, byte spans, приймач потоку |
| [Api.cpp](Api.cpp) | Єдиний owner, група `device` у `Model{...}`, handles, Workspace, resource providers і routing |
| [main.cpp](main.cpp) | Host клієнт і перевірки native/encoded/resource/stream шляхів |
| [CMakeLists.txt](CMakeLists.txt) | Окремий executable прикладу з потрібними compiled sources бібліотеки |

Модель — одна група `device` дерева груп: Fields, Commands і Services разом.
Native операції фасаду йдуть через handles, а опубліковані IDs з `Api.hpp`
перевіряються проти шляхів, як у [QuickStart.cpp](../user_guide/QuickStart.cpp)
і [group_tree](../group_tree/README.md):

```cpp
constexpr ts::Model model{ts::group("device",
    ts::field<&Device::readPeriod, &Device::writePeriod>("Period", device),
    ts::command<&Device::reset>("Reset", device),
    ts::service<&Device::sample>("Sample", device))};
constexpr auto period = ts::endpoint<model, "device/Period">;
static_assert(period.id == 0); // ID для власного протоколу
period.write(25u);
```

## Зібрати й запустити

З кореня репозиторію `telemetry`, із встановленими CMake, Ninja та C++20 compiler:

```sh
cmake -S examples/device_integration -B /tmp/telemetry-device-example -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/telemetry-device-example
ctest --test-dir /tmp/telemetry-device-example --output-on-failure
```

На Windows можна явно вибрати наявний Qt MinGW. Наведені шляхи відповідають
набору, яким перевірено цей приклад; на іншій машині вкажіть встановлені tools.
Виконайте з кореня `telemetry`:

```powershell
$env:PATH = 'C:/Qt/Tools/mingw1310_64/bin;' + $env:PATH
$exampleBuild = Join-Path $env:TEMP 'telemetry-device-example'
& C:/Qt/Tools/CMake_64/bin/cmake.exe -S examples/device_integration -B $exampleBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
& C:/Qt/Tools/CMake_64/bin/cmake.exe --build $exampleBuild
& C:/Qt/Tools/CMake_64/bin/ctest.exe --test-dir $exampleBuild --output-on-failure
```

Використайте каталог результатів поза репозиторієм. Успішний executable друкує
`Device integration guide: N checks passed`; кожен check перевіряє умову через
явний виклик, тому Release/NDEBUG не прибирає перевірки. Для спільної перевірки
всіх documentation examples є
[tests/docs/run.py](../../tests/docs/run.py):

```sh
python tests/docs/run.py --cxx g++ --build-dir /tmp/telemetry-doc-examples
```

Compiled sources тут рівно три: `lib/telemetry/abi/StructuredAbi.cpp`,
`lib/telemetry/model/Adapter.cpp`, `lib/resource/protocol/Protocol.cpp`.
Include directories: `lib`, `lib/boost_pfr/include`, `lib/magic_enum`.
CMake збирає саме приклад застосунку. Qt, sockets і HAL йому не потрібні.

## Що додавати у свій застосунок

`Device` зберігає `SamplingConfig` і `DisplayConfig`. Група `device`
публікує scalar `Period`, enum `Mode`, масив `Gains` та aggregate `Display`.
Всі чотири getters/setters — methods бізнес-класу. `Configure` передає два
блоки одним aggregate; `Device::configure()` перевіряє обидва до зміни стану.
`Reset` — команда без аргументів. `Sample` — сервіс без аргументів, а `Query`
має один aggregate request і повертає owning `Snapshot` або `InvalidArgument`.

Власні аргументи додавайте до aggregate request, а не до wire header. Нове поле
додавайте до групи і до відповідного публічного ID. Позиції — частина
адресації: перестановка entries змінює їхні IDs, тому клієнта треба оновити разом
із застосунком. ID кожного endpoint дає його шлях: `period.id` або
`ts::idOf<model, "device/Reset">()`; `static_assert` перевіряє, що публічні
runtime IDs фасаду збігаються з ними. У цьому прикладі одна група з позицією `0`.
Field/Command/Service мають окремі простори IDs, які вибирає operation.

Споживач включає лише `Api.hpp` і викликає звичайні runtime функції:

```cpp
#include "Api.hpp"
std::uint32_t period = 0;
if (app::api::readPeriod(period)) {
    const auto status = app::api::writePeriod(20);
    // Перевірити status == app::WriteStatus::Applied.
    (void)status;
}
```

У `.cpp` native фасад користується handles (`period.read()`, `mode.write(value)`);
encoded фасад передає
байтові payloads у `readFieldEncoded`, `writeFieldEncoded`,
`executeCommandEncoded`, `callServiceEncoded`. Перевірка wire payload і рішення
бізнес-класу мають різні результати. Наприклад, bool byte `2` дає
`Dispatch::InvalidPayload` до callback. Enum code `2` декодується як underlying
integer, після чого `Device::writeMode()` повертає `WriteStatus::InvalidValue`
при `Dispatch::Ok`. Reflection dictionary не підміняє domain validation.

`Device`, providers, paths, handles і Model мають static lifetime у `Api.cpp`.
Додатковий `init()` тут не потрібний. Фасад призначений для **одного** бізнес-owner;
кілька `StreamReceiver` можуть обслуговувати різні з'єднання з тим самим owner
за умови серіалізації викликів.

## Завершений запит і framing застосунку

Цей приклад обрав власний envelope. Це рішення застосунку, а не wire protocol
telemetry чи resource. Вхідний і вихідний frame мають форму:

```text
u16 little-endian bodyBytes | body[bodyBytes]
```

`bodyBytes` не включає двобайтовий prefix і має бути від `1` до `128`.
`StreamReceiver` накопичує prefix і body в масивах сталої місткості.
`onCompletePacket()` отримує лише ціле body. Фрагмент ще не викликає endpoint.
Одна порція input може містити частину frame, один frame або кілька frames.

| Route | Body запиту | Body відповіді |
| --- | --- | --- |
| `1`, Direct | `u8 route, u8 operation, u32 LE id, canonical payload` | `u8 route, u8 dispatch, u8 endpointStatus, canonical output` |
| `2`, Resource | `u8 route, complete resource packet` | `u8 route, complete resource reply` |
| Помилка envelope | Невідомий route або закороткий direct header | `0, 1` — application InvalidPacket |

Direct operation: Read=`1`, Write=`2`, Command=`3`, Service=`4`. Read не має
payload. Read/Write/Command/Service ID береться з відповідного public enum.
`endpointStatus` має значення лише при dispatch `Ok`: для Write це
`WriteStatus`, Command — `CommandStatus`, Service — service status; для Read
байт нульовий. Невідома direct operation дає dispatch `InvalidPayload`.
Результат `PacketStatus::Replied` означає сформовану відповідь, тому сам клієнт
також перевіряє dispatch/endpoint/resource status всередині body.

Наприклад, frame для Read Period: `06 00 01 01 00 00 00 00`.
При Period=`10` відповідь: `07 00 01 00 00 0A 00 00 00`.
Запити тут обробляються послідовно, кожен має одну відповідь; correlation ID,
контроль цілісності й policy повторів цей приклад не визначає. Повтор `Reset`
виконує команду знову. Повтор WRITE повторно доставляється provider.

Нульовий або завеликий length переводить receiver у failed state. Подальші
bytes не шукаються як новий header. `reset()` відкидає неповний input і failed
state на відомій новій межі потоку, зберігає `completedFrames()` та бізнес-стан.
Для TCP такою межею є нове з'єднання; для UART застосунок сам задає recovery
policy і timeout. Reset посеред потоку без встановленої межі може втратити
синхронізацію. Перевірка capacity відповіді відбувається до side effect:
наприклад, замалий WRITE reply не викликає provider.

## Resource provider і file paths

`resource::filesystem()` має дві flat labels: `/device/version.bin` та
`/device/note.bin`. Слеші є частиною label; tree, directory traversal і runtime
пошуку за path тут немає. LIST дає paths у порядку file indices. STAT повідомляє
size і read/write flags. READ/WRITE звертаються вже за index і cursor.

Version використовує `BytesFile` для сталої read-only послідовності.
`NoteFile` — власний provider з методами `size/read/write`, який тримає 16 bytes
RAM і споживає до двох bytes за WRITE. Після відповіді `consumed=2, complete=0`
клієнт відкидає лише ці два bytes і посилає suffix із поверненим `next` cursor.
`final=true` стосується поданого input; completion стає true, коли весь цей
input спожито. Це навчальна RAM policy, без flash commit або транзакції.

`onCompletePacket()` передає resource body без route byte прямо в
`resource::protocol::process()`. Формат внутрішнього LIST/STAT/READ/WRITE
зберігається без змін. Обидва route використовують один application receiver,
але resource packet не містить direct ID або direct operation header.

## Замінити host feeder на UART/TCP callback

Тримайте окремий `StreamReceiver` у стані кожного input stream. У callback
приймання передайте фактично отриманий span у `feed()`; packet boundary не
збігається з UART DMA block чи одним TCP read. Перед новим з'єднанням або після
встановленого timeout/recovery boundary викличте `reset()`.

Reply sink одержує цілий frame. Він має до повернення callback синхронно
скопіювати bytes у власну TX queue або повністю їх передати. Receiver
використовує цей самий response buffer для наступного frame; зберігати span
для пізнішого DMA send не можна. Queue capacity/backpressure обробляє транспорт
застосунку. Якщо queue не прийняла вже виконану команду, її side effect не
відкочується. Не робіть recursive `feed()` у цьому самому receiver.

У firmware відкладіть виклик фасаду в application task, якщо callbacks
надходять з interrupt context. Один task/lock має серіалізувати всі native та
encoded операції, resource доступ і diagnostics до спільного owner і Workspace.
Приклад не додає mutex і не обіцяє coherent snapshot під час одночасної зміни
стану. Якщо читання відповіді потребує такого snapshot, створіть його під тією
самою синхронізацією у бізнес-класі до encoded повернення.

## Що перевіряє executable

`main.cpp` перевіряє всі чотири форми Field у спільній таблиці, native і
encoded access, canonical bytes, validation до/після callback, застосування
двох configs одним Command, нуль/один aggregate argument у Commands/Services,
повтор команд, LIST/STAT/READ/WRITE з partial consumption і capacity refusal.
Stream checks подають split prefix, split payload, два повні frames одним
chunk, frame по одному byte, invalid lengths, reset після неповного input і
перевіряють точні reply bytes та counters. Host результат підтверджує ці шляхи;
апаратна UART/TCP швидкість, scheduling і DMA lifetime цим запуском не
встановлені.

Пояснення поряд із прикладом:
[Application integration](../../doc/user/ApplicationIntegration.md),
[Transport walkthrough](../../doc/user/TransportWalkthrough.md),
[Native API](../../doc/user/NativeApi.md),
[Transport and Resources](../../doc/user/TransportAndResources.md).

# Commands: прикладні дії та їхній результат

[API Overview](APIOverview.md) · [Native reference](NativeApi.md) ·
[Native-only приклад](../../examples/user_guide/NativeOnly.cpp)

> **Одна форма — дерево груп, шляхи й handles.** У Model із груп той самий
> endpoint оголошують усередині `ts::group(...)` і викликають за шляхом:
> `ts::endpoint<model, "adc/Reset">` далі `.call()`, або `ts::call<model, "adc/Reset">()`. Шлях однаковий на пристрої, у Static C++, TS/JS і Dynamic
> клієнті: [одна операція в чотирьох місцях](APIOverview.md#одна-операція-в-чотирьох-місцях).
> Приклади цієї сторінки, де є catalog tables і `makeId`, описують плоску форму, що
> [виводиться з ужитку](APIOverview.md#виводиться-з-ужитку); signatures, results і
> lifetime rules самих Field, Command і Service від форми Model не залежать.

`Command` відкриває дію: скинути лічильник, застосувати повний configuration
block, вибрати канал, почати збереження або передати роботу прикладній черзі.
Callback повертає `telemetry::CommandResult`. Command не має response payload;
якщо потрібні дані у відповіді, використовуйте [Service](Services.md).

Request — або відсутній, або одна ordinary aggregate struct. Її C++-члени
визначають форму canonical payload. Перевірка діапазонів, допустимих режимів,
стану пристрою та можливості виконати дію належить callback. Library не
створює task, queue, asynchronous completion tracking чи retry policy.

Це довідник поточного C++20 API. `ts` у прикладах означає
`namespace ts = telemetry;`. Загальний include —
[`<telemetry/Telemetry.hpp>`](../../lib/telemetry/Telemetry.hpp).
Declarations складаються з name та binding; прикладна семантика не
записується в structural descriptor.

Native declaration/local table не потребують Model або Workspace; catalogs
додають global IDs. Публічні family headers —
[`<telemetry/command/CommandTable.hpp>`](../../lib/telemetry/command/CommandTable.hpp) і
[`<telemetry/command/CommandCatalogs.hpp>`](../../lib/telemetry/command/CommandCatalogs.hpp).
Це не прибирає compile-time type/reflection validation чи транзитивні headers;
дивіться [Native access dependencies](NativeApi.md).

Для runtime ID використовуйте `commands.call(id[, request])`: результат —
один `CommandCallStatus`, що охоплює routing та application outcome.
Static `call<Id>()`, callback `CommandResult` і low-level `callAs` збережено
без змін. Повний опис: [Flat runtime API](FlatNative.md).

## 1. Повний приклад

Приклад показує no-request action, один validated configuration request,
global packed ID та encoded dispatch. Build requirements ті самі, що в
[runnable guide examples](../../examples/user_guide/README.md).

```cpp
#include <telemetry/Telemetry.hpp>
#include <array>
#include <cassert>
#include <cstdint>

namespace ts = telemetry;

enum class Mode : std::uint8_t { Off, Measuring };
struct Configure {
    std::uint32_t periodMs;
    Mode mode;
};

struct Device {
    Configure config{10, Mode::Measuring};
    unsigned resets = 0;
    bool busy = false;

    ts::CommandResult reset() noexcept {
        ++resets;
        return ts::CommandResult::Executed;
    }

    ts::CommandResult configure(const Configure& next) noexcept {
        if (next.periodMs < 1 || next.periodMs > 1000 ||
            (next.mode != Mode::Off && next.mode != Mode::Measuring))
            return ts::CommandResult::InvalidValue;
        if (busy)
            return ts::CommandResult::Busy;
        config = next;
        return ts::CommandResult::Executed;
    }
};

inline Device device;
inline constexpr ts::CommandTable localCommands{
    ts::command<&Device::reset>("Reset", device),
    ts::command<&Device::configure>("Configure", device),
};
inline constexpr ts::CommandCatalogTable commands{
    ts::group("control", localCommands),
};
inline constexpr ts::FieldCatalogTable<> fields{};
inline constexpr ts::ServiceCatalogTable<> services{};
inline constexpr ts::Model model{fields, commands, services};

int main() {
    assert(localCommands.call<0>() == ts::CommandResult::Executed);
    assert(localCommands.call<1>(Configure{20, Mode::Measuring})
           == ts::CommandResult::Executed);
    assert(localCommands.call<1>(Configure{0, Mode::Off})
           == ts::CommandResult::InvalidValue);
    assert(device.config.periodMs == 20);

    constexpr auto configureId = ts::makeId<0, 1>();
    std::array<std::byte, ts::wireSize<Configure>> input{};
    std::array<std::byte, model.maxCommandScratch()> storage{};
    ts::Workspace workspace{storage};
    assert(ts::encode(Configure{30, Mode::Off}, input) == ts::CodecStatus::Ok);
    const auto result = commands.index().executeEncoded(configureId, input, workspace);
    assert(result.dispatch == ts::DispatchStatus::Ok);
    assert(result.endpointStatus == ts::CommandResult::Executed);
    assert(device.config.periodMs == 30);
    assert(workspace.used() == 0);
}
```

Callback перевіряє обидва члени до mutation. Codec прийме representable
unknown `Mode` code, але callback відхилить його. Native та encoded paths
викликають ту саму business function і отримують однаковий application status.

Повні runnable приклади:
[Native.cpp](../../examples/user_guide/Native.cpp),
[Encoded.cpp](../../examples/user_guide/Encoded.cpp) та
[Device integration](../../examples/device_integration/README.md).

## 2. Підтримувані callback signatures

| Форма | Сигнатура | Native call |
| --- | --- | --- |
| Без request | `CommandResult reset() noexcept` | `definition.call()` |
| Request by value | `CommandResult configure(Configure value) noexcept` | `definition.call(value)` |
| Request by const reference | `CommandResult configure(const Configure& value) noexcept` | `definition.call(value)` |
| Const owner method | `CommandResult Owner::check(const Query&) const noexcept` | Borrowed const owner, якщо дія дозволяє const method |
| Lvalue-qualified owner method | `CommandResult Owner::configure(const Configure&) & noexcept` | Borrowed live lvalue owner |

`Request` виводиться з сигнатури. No-argument callback має `Request = void`
(`telemetry::Void` — alias `void`); empty aggregate request усе ще є struct,
а не Void. Request struct може містити fixed scalars, supported scoped enums,
`std::array` та nested aggregates. `std::array` чи scalar як весь request
не приймається: загорніть його в struct з meaningful member name.

Callback повертає exact `telemetry::CommandResult` by value. Не підтримуються
`void`, `bool`, integer або reference як заміна result; два чи більше arguments;
mutable/rvalue reference request, pointer request, volatile payload, variadic
або throwing callback, volatile owner method та rvalue-qualified owner method.
Request by value або exact const lvalue reference є єдиними shape forms.

Кілька business arguments об'єднуйте в одну request struct:
`struct Configure { std::uint32_t periodMs; Mode mode; };`.
Це одна operation з одним typed input. Command не інтерпретує positional
parameter lists, JSON values або generic scalar containers.

## 3. Усі форми declaration і target storage

Фабрика — `telemetry::command`; її результат — `CommandDefinition<Binding>`.
Name та inferred request shape залишаються незмінними при slot rebinding.

| Форма | Приклад | Що retained |
| --- | --- | --- |
| Known free function | `command<&resetFree>("Reset")` | Target у binding type |
| Known owner method | `command<&Device::configure>("Configure", device)` | Borrowed object address та method target |
| Owner wrapper | `command<&Device::configure>("Configure", std::ref(device))` | Borrowed underlying owner |
| Const owner wrapper | `command<&Device::check>("Check", std::cref(device))` | Borrowed const owner для compatible const method |
| Rebindable owner | `command<&Device::configure>("Configure", ownerSlot)` | Borrowed `OwnerSlot<Device>` |
| Runtime function pointer | `command("Reset", &resetFree)` | Копія exact function pointer |
| Capture-free lambda | `command("Reset", []() noexcept { return ts::CommandResult::Executed; })` | Копія converted native function pointer |
| Named functor | `command("Configure", functor)` | Borrowed callable lvalue |
| Named capturing closure | `command("Configure", closure)` | Borrowed closure lvalue |
| Callable wrapper | `command("Configure", std::ref(closure))` | Borrowed underlying callable |
| Callable slot | `command("Configure", configureSlot)` | Borrowed stable slot address |

Direct owner — actual live object або compatible derived object. Pointer
та smart-pointer variables не є owner binding; передайте перевірений
`*pointer`, якщо object lifetime гарантовано, або використовуйте `OwnerSlot`.
Constness зберігається. Temporary object, converting proxy та temporary
capturing closure відхиляються на factory boundary. Для Field/Command
explicit owner/callable type parameters не використовуються: factory має
вивести actual argument category сама.

Runtime function pointer копіюється; зміна вихідної pointer variable після
declaration не змінює target. Named callable не копіюється. Capture-free
temporary lambda може бути перетворена й збережена як exact pointer.
Overloaded method оберіть explicit member-pointer cast; generic/overloaded
functor обгорніть typed lambda або передайте через exact callable slot.
Callback та pointer conversion повинні бути nothrow.

Factory не приймає units, limits, default arguments або окрему request
metadata list. Name — non-null, nonempty valid UTF-8 C-string із bounded
length. Name memory borrowed та immutable. Invalid factory name є
definition contract violation: constexpr diagnostic або runtime `abort`.
Duplicate names перевіряє descriptor у межах відповідного catalog, а
runtime command lookup використовує declaration positions.

### Slot signatures та bind forms

No-request command slot має `CommandResult() noexcept`; command із request —
`CommandResult(Configure) noexcept` або
`CommandResult(const Configure&) noexcept`.

| Slot | Bind API / declaration | Ownership |
| --- | --- | --- |
| `OwnerSlot<Device>` | `slot.bind(device)`; `command<&Device::configure>("Configure", slot)` | Borrowed owner |
| `FunctionSlot<Signature>` | `slot.bind(&configureFree)`; `command("Configure", slot)` | Exact pointer snapshot |
| `ContextFunctionSlot<Signature>` | `slot.bind(callback, context)` | Callback actual signature починається з `void*`; context borrowed |
| `DelegateRefSlot<Signature>` | `bind<&freeFunction>()`, `bind<&Device::configure>(device)`, `bind(namedCallable)` | Borrowed callable/owner або free function target |
| `DelegateSlot<Signature, Bytes, Align>` | `bind([&device](const Configure& r) noexcept { return device.configure(r); })` | Owned inline closure |

Default owned capacity — 32 bytes, alignment — `alignof(std::max_align_t)`.
Bind copies lvalue closures або moves rvalue closures; construction/movement/
destruction та invocation nothrow. Target має fit в inline storage;
oversized target не переходить на heap. Owned closure не володіє її
reference-captured owners. Усі slots noncopyable/nonmovable і borrowed
definition зберігає stable slot address.

Усі slots мають `reset`, `available`, `explicit operator bool`, `get`.
Callable slot `invoke` вимагає engaged target; safe endpoint call спочатку
перевіряє availability. Function/context snapshots є copied values; delegate
snapshots є views без lifetime extension. Bind/reset/destruction мають бути
serialized із call. Owned callback не може reset/replace власний slot
під час свого виконання. Повні overloads і signature resolution —
[runtime slots](../../lib/telemetry/slot/README.md).

## 4. CommandDefinition та exact native call

| API | Тип | Призначення |
| --- | --- | --- |
| `Request` | Aggregate type або `void` | Inferred native request |
| `name()` | `const char*` | Borrowed declaration name |
| `call()` | `CommandResult` | Виклик no-request command |
| `call(request)` | `CommandResult` | Exact request type, category forwarded |

Argument type після зняття cv/ref має точно збігатися з `Request`; implicit
numeric чи user-defined conversion до іншої struct не приймається. Const
request lvalue й request rvalue дозволені, якщо callback signature callable
з ними. Mutable request reference як callback declaration не підтримується.
У Command немає `read`, `write`, `readAs`, `writeAs` або conversion call API.

Target snapshots один раз: availability check та invocation використовують
той самий selected target. Empty slot, empty owner slot, null runtime function
pointer або відсутній linked weak target повертає native `Unavailable` без
application callback. Direct known owner object не одержує runtime null-owner
check: його lifetime є precondition.

Native `call` повертає callback status напряму. Shape/arity/type errors
відомого native call дають compile-time diagnostic; вони не перетворюються
на runtime `ArgumentCountMismatch`. Callback може використати цей supported
status для своєї прикладної семантики. Encoded length mismatch натомість є
`DispatchStatus::InvalidPayload`.

## 5. Tables, catalogs, ID та traversal

`CommandTable` зберігає різні declarations у declaration order і генерує
їхні erased rows. Local position — zero-based integer або scoped enum.
`CommandCatalogTable` позичає local tables через `group(name, table)`;
catalog declaration order задає group position.

| API | Результат | Призначення |
| --- | --- | --- |
| `local.get<Position>()` | `const CommandDefinition&` | Exact declaration |
| `local.call<Position>()` | `CommandResult` | No-request native action |
| `local.call<Position>(request)` | `CommandResult` | Exact typed native request |
| `local.call(position)`, `call(position, request)` | `CommandCallStatus` | Рекомендований runtime виклик з одним outcome |
| `local.callAs(position)`, `callAs(position, request)` | `NativeCallResult<CommandResult>` | Low-level routing та application status окремо |
| `catalogs.get<Id>()` | `const CommandDefinition&` | Static global declaration |
| `catalogs.call<Id>()`, `call<Id>(request)` | `CommandResult` | Static global native action |
| `catalogs.call(id)`, `call(id, request)` | `CommandCallStatus` | Рекомендований runtime packed-ID виклик |
| `catalogs.callAs(id)`, `callAs(id, request)` | `NativeCallResult<CommandResult>` | Low-level routing та application status окремо |
| `size()`, `empty()` | `size_t`, `bool` | Local entry count / catalog group count |
| `data()`, `begin()`, `end()` | Borrowed row pointers | Erased iteration без execution |
| `operator[](i)` | `const CommandEntry&` / `const CommandCatalog&` | Unchecked; requires `i < size()` |
| `forEach(visitor)` | `void` | Typed traversal усіх declarations у порядку |
| `forEachWhile(visitor)` | `bool` | Typed traversal до exact-`bool` false |
| `catalogs.forEachEntry(visitor)` | `void` | Packed ID, group name та erased CommandEntry |
| `catalogs.forEachEntryWhile(visitor)` | `bool` | Erased обхід до exact-`bool` false |
| `visit(position_or_id, visitor)` | `bool` | Checked typed runtime selection |
| `catalogs.index()` | `CommandIndex` | Copyable borrowed runtime index |

Local table не має `index()`. Для local encoded access використовуйте
checked method обраної `CommandEntry`; для global external ID —
`catalogs.index().executeEncoded`.

`PackedId` — u32: high 16 bits group, low 16 bits entry.
`makeId<0, 1>()` обирає другий command першого catalog. Command, Field та
Service мають окремі logical ID spaces: однакове числове ID допустиме в
різних families, і transport operation має обрати потрібну family.
Endpoint names не замінюють IDs. Reordering declarations змінює identity.

Static API invalid ID/position дає diagnostic. Runtime global APIs
приймають integral packed ID, не local scoped enum, і перевіряють original
width до conversion: negative ID та value ширше u32 відхиляються.
`tryMakeId(group, entry)` — fallible components; `makeId<Group, Position>()`
— гарантовані static checks. `makeId(group, entry)` invalid runtime
components є contract violation. `tryGroupOf`/`tryIndexOf` перевіряють packed
range й повертають optional components; `groupOf`/`indexOf` вимагають valid
range. Lookup окремо перевіряє існування catalog/row.

Local `forEach` приймає `visitor(definition)` або optional typed
`operator()<Position>(definition)`. Global `forEach` приймає
`visitor(catalogName, definition)` або
`operator()<Group, Entry>(catalogName, definition)`. Обидва `visit`
передають лише `visitor(definition)`, без catalogName. Callback return
ігнорується: boolean `visit` означає selection, а не execution success.
Traversal сам по собі не викликає command; command виконається тільки тоді,
коли ваш visitor викличе `definition.call`.
`forEachWhile` підтримує ті самі typed visitor forms; `forEachEntry` передає
metadata, не виконує Command. While-result true означає повний обхід, включно
з empty table, false — requested stop. Деталі:
[Tables and catalogs](TablesAndCatalogs.md#обійти-definitions-або-metadata).

### Typed call з runtime ID

Рекомендована форма повертає один status:

```cpp
const Configure request{40, Mode::Measuring};
const ts::PackedId id = ts::makeId<0, 1>();
const auto status = commands.call(id, request);
assert(status == ts::CommandCallStatus::Executed);
assert(localCommands.call(0) == ts::CommandCallStatus::Executed);
```

`CommandCallStatus` зберігає всі defined application outcomes: `Executed`,
`Accepted`, `NotFound`, `Unavailable`, `ArgumentCountMismatch`, `InvalidValue`,
`Busy`, `Failed`; routing mismatch додає `SignatureMismatch`. Неприпустимий
або відсутній ID дає `NotFound`; wrong Request/no-request форма дає
`SignatureMismatch` до callback. Empty compatible slot дає `Unavailable`.
Невизначений numeric callback code відображається у `Failed`; low-level
`callAs` нижче зберігає його raw value. Enum `CommandResult` і wire values
не змінюються. `Accepted` означає прийняту роботу, без встановленого completion.
Обидві runtime форми вимагають lvalue table/catalog та перевіряють width
до narrowing. Детальні mapping rules: [Flat runtime API](FlatNative.md).

### Low-level routing та application status

`callAs(position_or_id[, request])` обирає runtime Command і повертає
`NativeCallResult<CommandResult>`. Request має точно збігатися з declared
`Request` після зняття cv/ref; conversions між structs чи numeric members
не виконуються. Форму без request можна застосувати тільки до `Request = void`.
Невідповідність дає `SignatureMismatch` до callback і slot availability check.

Для `Configure` із першого повного прикладу visitor можна замінити direct
runtime call. Раніше selection і endpoint status оброблялися окремо:

```cpp
ts::CommandResult outcome = ts::CommandResult::NotFound;
const Configure request{40, Mode::Measuring};
const ts::PackedId id = ts::makeId<0, 1>();
const bool selected = commands.visit(id, [&](const auto& definition) {
    using Definition = std::remove_cvref_t<decltype(definition)>;
    if constexpr (std::is_same_v<typename Definition::Request, Configure>)
        outcome = definition.call(request);
    else
        outcome = ts::CommandResult::ArgumentCountMismatch;
});
```

Усі generated visitor branches повинні компілюватися, тому `if constexpr`
відділяє exact request types. `selected == false` залишить `NotFound`;
`selected == true` ще не означає `Executed`. Traversal visitors не повинні
бути noexcept, але endpoint callback signatures — повинні.

Та сама операція через збережений low-level `callAs`:

```cpp
const Configure request{40, Mode::Measuring};
const ts::PackedId id = ts::makeId<0, 1>();
const auto result = commands.callAs(id, request);
assert(result.status() == ts::NativeCallStatus::Ok);
assert(result.hasValue());
assert(result.value() == ts::CommandResult::Executed);

const auto reset = localCommands.callAs(0);
assert(reset.hasValue() && reset.value() == ts::CommandResult::Executed);
```

| `NativeCallStatus` | Meaning |
| --- | --- |
| `Ok` | Exact endpoint result доставлено; application status читається через `value()` |
| `NotFound` | Position/ID invalid на original width або group/row відсутня |
| `SignatureMismatch` | Row існує, але exact Request або no-request форма не збігається |

`result.hasValue()` означає доставку `CommandResult`, а не `Executed`.
`Busy`, `InvalidValue` та `Unavailable` збережено у `result.value()` при
selection `Ok`; наприклад empty declared slot дає `Ok + Unavailable`.
`value()` вимагає selection `Ok`; `valueOrNull()` повертає null на failure.
Pointer вказує на `CommandResult`, який належить outer result: named result
має жити до останнього pointer use. `valueOrNull()` доступний лише для `&` /
`const&`; overloads `&&` / `const&&` явно видалені, також для immediate-use
виклику на temporary. `value()` зберігає попередні return categories й дозволяє
move-out із rvalue result. Для Command зазвичай достатньо скопіювати невеликий
enum через `auto outcome = result.value();`. Pointer guard є compile-time
обмеженням і не додає runtime checks чи storage.
Ці native selection statuses не є encoded dispatch чи wire codes.

Local input приймає integer або scoped position enum; packed ID — integral.
Negative/wide input перевіряється до narrowing. Методи вимагають lvalue
table/catalog; names, slots та owners зберігають свої borrowed lifetimes.
`callAs` виконується синхронно і не додає queue чи request ownership.

Typed `visit` лишається варіантом для кількох request types, custom result
handling або доступу до definition. Static `call<Position/Id>` не змінюється;
encoded bytes викликаються через `executeEncoded`. Runnable coverage:
[NativeCalls.cpp](../../tests/ergonomics/NativeCalls.cpp).

`RootTypes` містить один request type на кожну Command у порядку оголошення,
включно з `Void` для команд без запиту; повтори зберігаються.
`RegistryRootTypes` прибирає повтори, зберігаючи перше входження, і служить
лише compile-time входом Registry. Каталог має обидва aliases; positional
roots охоплюють усі групи. `staticSize` рахує рядки локальної таблиці або
групи каталогу, а `TypeStorage<Registry>` має TypeId для кожної команди.
Це structural metadata, не active requests. `model.view().commandTypeId(id)` повертає
optional request TypeId; Void request має registry Void type.
TypeId та packed endpoint ID є різними identities.

## 6. Encoded execution

Transport має спершу встановити межу повного request. Потім передайте його
canonical payload і Workspace. Вхідний payload не містить library-owned
operation/header/session: ці поля обробляє application adapter.
Byte contract — [WireV3](../WireV3.md), receive/reply приклад —
[TransportWalkthrough](TransportWalkthrough.md).

| API | Результат | Значення |
| --- | --- | --- |
| `CommandEntry::executeEncoded(input, workspace)` | `EncodedCommandResult` | Checked operation однієї row |
| `CommandIndex::find(id)` | `const CommandEntry*` | Checked lookup; nullptr для invalid/absent ID |
| `CommandIndex::executeEncoded(id, input, workspace)` | `EncodedCommandResult` | Lookup та checked execution |
| `CommandIndex::catalogs()`, `count()` | Catalog pointer, `uint32_t` | Borrowed catalogs та group count |
| `executeCommandEncoded(modelView, id, input, workspace)` | `EncodedCommandResult` | Compiled erased Model adapter |

`CommandEntry` містить `name`, `requestWireBytes`, `scratchBytes`, `context`
та raw `invoke` pointer. Raw pointer потребує всіх checks checked method;
application code має використовувати `executeEncoded`. Hand-built index
вимагає live coherent catalog pointers/count: lookup не validates їхню
пам'ять. Index/table views позичають backing storage.

Input length має бути точно `requestWireBytes`; no-request command вимагає
empty input. Empty struct також має zero wire bytes, але native API все ще
приймає struct object. Invalid bool representation та unsupported byte length
відхиляються до target invocation. Enum dictionary не фільтрує
representable unknown codes — їх перевіряє business callback.

Small decoded request розміщується local за compile-time storage policy;
large request використовує aligned Workspace lease. `scratchBytes` включає
worst-case alignment, а `model.maxCommandScratch()` достатній для однієї
операції зі fresh Workspace. Якщо scratch потрібен, input span не може
overlap Workspace storage. Lease lifetime обмежений викликом; callback,
що ставить work у queue, має скопіювати потрібні request data до своєї
owned storage. Він не може зберегти reference на decoded request після return.

## 7. Результати й порядок handling

| `CommandResult` | Code | Meaning |
| --- | --- | --- |
| `Executed` | 0 | Application виконала дію синхронно |
| `Accepted` | 1 | Application прийняла роботу, наприклад у власну queue; завершення не встановлено |
| `NotFound` | 2 | Supported application outcome; runtime index absence окремо є dispatch NotFound |
| `Unavailable` | 3 | Native binding unavailable або application не надає дію зараз |
| `ArgumentCountMismatch` | 4 | Supported callback outcome; native signature errors є compile-time errors |
| `InvalidValue` | 5 | Business validation відхилила request |
| `Busy` | 6 | Application зараз зайнята |
| `Failed` | 7 | Application виконання завершилося невдачею |

| `DispatchStatus` | Code | Encoded meaning |
| --- | --- | --- |
| `Ok` | 0 | Callback status доставлено, у тому числі application refusal |
| `NotFound` | 4 | Invalid/absent packed ID |
| `InvalidPayload` | 5 | Length, representation або scratch overlap refusal |
| `BufferTooSmall` | 6 | Shared dispatch code; Command payload API не має output span і його не генерує |
| `WorkspaceTooSmall` | 7 | Немає aligned storage для decoded request |
| `InternalError` | 8 | Callback повернув невизначений CommandResult code |
| `Unavailable` | 9 | Binding target не resolved до invocation |

`EncodedCommandResult` містить `dispatch` та `endpointStatus`; response bytes
і `written` у ньому відсутні. Спочатку перевірте dispatch. Тільки `Ok`
дозволяє інтерпретувати endpointStatus. `Ok + Busy/InvalidValue/Failed`
показує application refusal, а не codec failure. Definition/static calls та
low-level `callAs` зберігають raw callback status; flat runtime `call` застосовує
mapping до `CommandCallStatus`. Encoded execution перевіряє,
що callback status є одним із defined enumerators.

`Accepted` не означає завершення. Збережіть queued request у application-owned
memory, відкрийте окремий Field або Service для progress/result, якщо він
потрібний. Library не видає ticket, не очікує worker і не переводить
Accepted у Executed.

Виконання callback не є rollback transaction. Якщо callback вже змінив state
перед тим, як повернути `Failed` чи unknown status, encoded boundary не
відкотить ці зміни. Щоб request був all-or-nothing на рівні application,
validate усі члени та потрібні ресурси до першої mutation.

## 8. Lifetime, threads та повторні запити

Owner, callable lvalue, name memory, slot, local table та catalog повинні
залишатися живими за stable addresses. Local/catalog tables
noncopyable/nonmovable; row contexts можуть pointing у table-owned binding.
Borrowing `data`, `get`, traversal та `index` APIs require lvalue table.
Copied ModelView/CommandIndex не подовжує backing lifetime.

Native request живе щонайменше весь call. Encoded decoded request живе до
кінця operation; borrowed references не переносяться в asynchronous queue.
Bind/reset, owner mutation, calls та destruction мають бути externally
serialized. Library не додає locks або deferred deletion.

Один `call` означає одну спробу callback execution. Повтор того самого
canonical packet викликає command знову. Після transport timeout outcome
може бути невідомим: request міг виконатися, а reply — не надійти. Correlation,
repeat suppression, sequence policy та reconnect behavior має визначити
ваш adapter. Readiness/Bind із
[structured protocol example](../../examples/structured_protocol/README.md)
є optional transport-owned policy, не стан Command.

## 9. Джерела та runnable evidence

- [CommandDefinition та factories](../../lib/telemetry/command/Command.hpp).
- [CommandTable та CommandEntry](../../lib/telemetry/command/CommandTable.hpp).
- [CommandCatalogTable та CommandIndex](../../lib/telemetry/command/CommandCatalogs.hpp).
- [Native statuses](../../lib/telemetry/result/EndpointStatus.hpp),
  [encoded results](../../lib/telemetry/result/EndpointResults.hpp),
  [compiled adapter](../../lib/telemetry/model/Adapter.hpp).
- [Native.cpp](../../examples/user_guide/Native.cpp): free/method/callable
  bindings, exact requests, global call та typed visitor.
- [Encoded.cpp](../../examples/user_guide/Encoded.cpp): repeated command
  execution, malformed payload refusal та application counters.
- [Device.cpp](../../examples/device_integration/Device.cpp) і
  [Api.cpp](../../examples/device_integration/Api.cpp): business validation
  окремо від tables, framed packet assembly та complete request routing.
- [Fields](Fields.md) для current state й [Services](Services.md) для
  request/response та application status з data payload.

Authors: Ruslan Kovtun (shpegun60), codexAi. [MIT](../../LICENSE).

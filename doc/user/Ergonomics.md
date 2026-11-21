# Runtime native API та зручний обхід

[API Overview](APIOverview.md) · [Посібник](README.md) · [Fields](Fields.md) · [Commands](Commands.md) ·
[Services](Services.md) · [Runtime без nested results](FlatNative.md) · [Каталоги](TablesAndCatalogs.md) ·
[Повна runnable програма](../../examples/user_guide/Ergonomics.cpp)

> **Одна форма — дерево груп, шляхи й handles.** У Model із груп той самий
> endpoint оголошують усередині `ts::group(...)` і викликають за шляхом:
> `ts::endpoint<model, "adc/Config">` далі `.read()`, або `ts::read<model, "adc/Config">()`. Шлях однаковий на пристрої, у Static C++, TS/JS і Dynamic
> клієнті: [одна операція в чотирьох місцях](APIOverview.md#одна-операція-в-чотирьох-місцях).
> Приклади цієї сторінки, де є catalog tables і `makeId`, описують плоску форму, що
> [виводиться з ужитку](APIOverview.md#виводиться-з-ужитку); signatures, results і
> lifetime rules самих Field, Command і Service від форми Model не залежать.

Цей розділ збирає типові операції, для яких не потрібно писати власний typed
visitor. Всі доповнення працюють із поточними таблицями, positional IDs і
bindings; encoded wire v3 та правила володіння залишаються спільними.
Native access нижче не потребує Model або Workspace objects. Початок із
local tables — [NativeOnly.cpp](../../examples/user_guide/NativeOnly.cpp);
повні public headers і залежності — [Native reference](NativeApi.md).

## Вибрати API за завданням

| Потрібно | API | Результат |
| --- | --- | --- |
| Прочитати й явно скопіювати/перетворити | `fields.readAs<T>(id)` | `optional<T>` |
| Те саме з причиною відмови | `fields.readAsResult<T>(id)` | `FieldReadResult<T>` |
| Отримати const view існуючого значення | `fields.readBorrowed<T>(id)` | `BorrowedValue<T>` |
| Явно перетворити input і записати Field | `fields.writeAs(id, value)` | `WriteResult` |
| Виконати Command із native Request | `commands.call(id, request)` | `CommandCallStatus` |
| Викликати owning Service | `services.callAs<Reply>(id, request)` | `ServiceCallResult<Reply>` |
| Позичити response Service | `services.callBorrowed<Reply>(id, request)` | `BorrowedServiceCallResult<Reply>` |
| Service без response payload | `services.callAs<void>(id[, request])` | `ServiceCallResult<void>` |
| Окремо прочитати Command routing та application status | Low-level `commands.callAs(id, request)` | `NativeCallResult<CommandResult>` |
| Окремо прочитати Service routing та application result | Low-level `services.callAs<Result>(id, request)` | `NativeCallResult<Result>` |
| Обійти typed definitions до зупинки | `table.forEachWhile(visitor)` | `bool`: повний обхід |
| Обійти плоскі erased rows із ID | `catalogs.forEachEntry(visitor)` | `void` |
| Те саме до зупинки | `catalogs.forEachEntryWhile(visitor)` | `bool`: повний обхід |

Для local table замість packed `id` передається local position, включно зі
scoped position enum. Existing `call<Position/Id>()`, `read<Position/Id>()`,
`readAs`, `writeAs`, `forEach` і `visit` зберігають свої контракти.
Field `readAs/readAsResult/writeAs` допускають лише documented checked numeric
conversions або exact structural T. Command/Service dispatch натомість
вимагає exact Request/Response/ownership; схожі structs або conversion
members не роблять signatures сумісними. Це різні операції, а не одна
generic система implicit conversions.

## Runtime виклик з одним result

```cpp
const auto status = commands.call(id, Configure{25});
auto response = services.callAs<Reply>(queryId, request);
if (response)
    use(response.value());
else
    report(response.status());

auto config = services.callBorrowed<Big>(configId, request);
if (config)
    use(config.value());
```

Для no-request endpoint пропустіть request argument. Owning та borrowed
Service results мають один `hasValue()` / explicit `bool` і один `value()`:
успіх означає successful routing **та** application success. `status()` дає
`ServiceCallStatus`, а `valueOrNull()` повертає null при будь-якій відмові.
Для `void` є success-only `value()` без payload, але немає `valueOrNull()`.
`CommandCallStatus` охоплює application outcomes, `NotFound` і
`SignatureMismatch`; `Accepted` не встановлює completion.

Request, response та owning/borrowed policy мають збігатися точно; mismatch
не викликає callback. Фасад не додає owning response copy/move чи heap.
Borrowed `value()` дає `const T&` із попередніми lifetime/synchronization rules.
Повні статуси та runnable приклад: [Flat runtime API](FlatNative.md).

## Low-level native routing та application result

Наступні форми збережено без змін для окремого доступу до routing та
application outcome. Для звичайного runtime виклику використовуйте flat API вище.

```cpp
auto r = commands.callAs(id, Configure{25});
if (r.status() == telemetry::NativeCallStatus::Ok) {
    const telemetry::CommandResult outcome = r.value();
    // outcome може бути Executed, Accepted, Busy, Unavailable тощо.
}
```

`NativeCallStatus` має три значення:

| Status | Значення | Callback |
| --- | --- | --- |
| `Ok` | Exact endpoint result доставлений | Розв'язання binding виконано |
| `NotFound` | Неприпустима/відсутня позиція чи packed ID | Не викликається |
| `SignatureMismatch` | Row існує, але Request/Result відрізняється | Не викликається |

`hasValue()` зовнішнього result означає доставлений **результат endpoint**, а
не успішну прикладну дію. Порожній slot повертає outer `Ok` з inner
`CommandResult::Unavailable` або `ServiceStatus::Unavailable`.

Для Service Result задається явно, включно з ownership:

```cpp
auto owning = services.callAs<telemetry::ServiceResult<Reply>>(id, request);
auto borrowed = services.callAs<telemetry::BorrowedServiceResult<Big>>(id, request);
auto noPayload = services.callAs<telemetry::ServiceResult<void>>(notifyId);
```

Це три окремі сценарії; потрібна row має відповідати exact Request та Result.
Різні structs однакової форми не взаємозамінні; числових конверсій у callAs
немає. Для Command без Request використайте `commands.callAs(id)`.

`NativeCallResult<Result>::successFrom(factory)` конструює Result безпосередньо
у кінцевому union storage. Failure не конструює Result. Великий owning result
все одно займає свій розмір у вибраному caller storage: native виклик не
приховує heap або Workspace. Для великих відповідей callback зберігає
`ServiceResult<T>::successFrom(factory)`; encoded storage budget не змінено.
Explicit copy/move result користувачем залишається explicit copy/move.
Будь-який pointer на owned payload потребує живого named result.
`ServiceResult`, `NativeCallResult`, `FieldReadResult` та owning flat Service
result мають lvalue-only `valueOrNull()`: `&&` / `const&&` overloads видалені.
Owning Field `operator->()` має те саме обмеження, навіть для immediate-use
temporary expression. Збережіть result, перевірте success, потім отримайте
pointer чи використайте arrow. `value()` і Field `operator*()` зберігають
попередні return categories та rvalue move-out; отримана reference не продовжує
lifetime. Це source contract без додаткових runtime checks або storage.
Borrowed wrappers не володіють зовнішнім T: їхній temporary pointer access
лишається допустимим за lifetime contract backing object.
Пояснення pointer levels — [Services](Services.md#low-level-exact-runtime-callas),
owning Field lifetime — [Fields](Fields.md#runtime-borrowed-read-та-причина-відмови).

## Borrowed Field та причина невдалого owning read

```cpp
auto config = fields.readBorrowed<Config>(id);
if (config)
    use(config.value());

auto result = fields.readAsResult<std::uint16_t>(id);
if (!result.hasValue())
    report(result.status());
```

`readBorrowed<T>` вимагає exact declared `T` і getter з `const T&`. Runtime
невідповідність чи owning getter дає empty view **до getter**; static selection
має diagnostic. Конверсій, object copy та Workspace lease немає. View не
подовжує lifetime і не фіксує snapshot: зміна owner змінює видимі дані.

`readAsResult<T>` повертає owning result; status:

| Status | Причина |
| --- | --- |
| `Ok` | Значення отримано |
| `NotFound` | Позиції/ID немає |
| `TypeMismatch` | Structural types несумісні |
| `Unavailable` | Getter/binding недоступний |
| `ConversionFailed` | Checked numeric conversion відхилена |

Getter викликається один раз. Type mismatch перевіряється до availability;
для failure `T` не конструюється. Wrapper резервує місце для owning `T`, тому
для великого існуючого об'єкта використайте borrowed form, якщо копія не потрібна.

## Обхід із ID та зупинка

```cpp
fields.forEachEntry([](telemetry::PackedId id, std::string_view group,
                       const telemetry::FieldEntry& row) {
    show(id, group, row.name);
});

bool completed = fields.forEachWhile([](std::string_view group,
                                        const auto& definition) -> bool {
    return inspect(group, definition); // false завершує весь обхід.
});
```

Flat callbacks отримують metadata rows, не typed definitions чи live values.
Ті самі `forEachEntry`/`forEachEntryWhile` є у `FieldIndex`, `CommandIndex` та
`ServiceIndex`: наприклад, `model.view().fields.forEachEntry(visitor)`.
Порядок — group, потім row; packed ID формується із цих позицій. Empty groups
пропускаються. Сам обхід не викликає getter/Command/Service.

`forEachWhile` зберігає звичайні callbacks та optional `<Position>` або
`<Group, Position>` форми typed visitor. Callback має повертати саме `bool`.
`true` result означає повний обхід, `false` — requested stop; empty table
дає `true`. Зупинка припиняє runtime invocation, але всі typed branches все
одно інстанціюються. Existing `forEach` продовжує ігнорувати callback result.

## Розміри caller-owned буферів

Це окреме питання **encoded/transport** access, не вимога native API вище.
`Model::maxCommandRequestWireSize()`, `maxServiceRequestWireSize()`,
`maxServiceResponseWireSize()` і `maxScratch()` та їхні compile-time/runtime
contracts наведені в [Model API](Model.md#model-api). Вони описують одну
операцію без transport framing; nested operations потребують окремого
одночасного storage budget. Для resource envelopes використовуйте constants
та builders із [resource client API](../../lib/resource/protocol/README.md#bounded-client-api).

## Перевірка

[Ergonomic runner](../../tests/ergonomics/README.md) перевіряє native selection,
borrowing, числові відмови, strict traversal callbacks, resource client parsing,
128/256 targets та individual ARM stack frames для великих response. ARM
compile/disassembly не є виміром циклів на платі.

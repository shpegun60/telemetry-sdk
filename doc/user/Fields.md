# Fields: значення, читання та запис

[API Overview](APIOverview.md) · [Native reference](NativeApi.md) ·
[Native-only приклад](../../examples/user_guide/NativeOnly.cpp)

> **Одна форма — дерево груп, шляхи й handles.** У Model із груп той самий
> endpoint оголошують усередині `ts::group(...)` і викликають за шляхом:
> `ts::endpoint<model, "adc/Config">` далі `.read()` і `.write(value)`, член — `ts::read<model, "adc/Config.rateHz">()`. Шлях однаковий на пристрої, у Static C++, TS/JS і Dynamic
> клієнті: [одна операція в чотирьох місцях](APIOverview.md#одна-операція-в-чотирьох-місцях).
> Плоску форму з catalog tables, публічний `makeId` і typed доступ за ID
> [прибрано](APIOverview.md#прибрано): runtime ID обробляють `model.fields()`,
> `model.commands()` і `model.services()`, а catalogs позначають саме їх. Signatures,
> results і lifetime rules самих Field, Command і Service від форми Model не залежать.

`Field` відкриває одне прикладне значення через getter і, за потреби,
setter. Це може бути напруга, лічильник, режим, масив коефіцієнтів або ціла
структура налаштувань. Тип визначається C++-сигнатурою getter. Setter приймає
саме цей тип і вирішує, чи можна застосувати нове значення.

Один Field може містити багато членів структури. Reflection описує їхню
форму для codec і descriptor; окремих getter/setter для кожного члена
автоматично не з'являється. Для атомарного прикладного оновлення кількох
налаштувань зручно відкрити одну структуру й перевірити її всю перед записом.

Цей довідник описує поточний C++20 API. У прикладах `ts` — локальний alias
`namespace ts = telemetry;`. Загальний include —
[`<telemetry/Telemetry.hpp>`](../../lib/telemetry/Telemetry.hpp).
Назви та bindings є частиною declaration; units, допустимі межі, defaults,
персистентність і synchronization визначає застосунок.

Native FieldDefinition або local `FieldTable` працюють без Model/Workspace;
catalog потрібний лише для global IDs. Публічні family headers —
[`<telemetry/field/FieldTable.hpp>`](../../lib/telemetry/field/FieldTable.hpp) і
[`<telemetry/field/FieldCatalogs.hpp>`](../../lib/telemetry/field/FieldCatalogs.hpp).
Compile-time supported-type та reflection requirements залишаються чинними;
дивіться [залежності Native access](NativeApi.md).

## 1. Повний приклад з native та encoded доступом

Цю невелику програму можна включити в той самий build, що й приклади
[посібника](../../examples/user_guide/README.md). Вона показує точний запис,
позичене читання структури та explicit conversion:

```cpp
#include <telemetry/Telemetry.hpp>
#include <array>
#include <cassert>
#include <cmath>

namespace ts = telemetry;

struct Config {
    float limit;
    bool enabled;
};

struct Meter {
    Config config{250.0f, true};

    float readLimit() const noexcept { return config.limit; }
    const Config& readConfig() const noexcept { return config; }

    ts::WriteResult setLimit(float value) noexcept {
        if (!std::isfinite(value) || value < 1.0f || value > 1000.0f)
            return ts::WriteResult::InvalidValue;
        config.limit = value;
        return ts::WriteResult::Applied;
    }

    ts::WriteResult setConfig(const Config& next) noexcept {
        if (!std::isfinite(next.limit) || next.limit < 1.0f || next.limit > 1000.0f)
            return ts::WriteResult::InvalidValue;
        config = next;
        return ts::WriteResult::Applied;
    }
};

inline Meter meter;
inline constexpr ts::FieldTable localFields{
    ts::field<&Meter::readLimit, &Meter::setLimit>("Limit", meter),
    ts::field<&Meter::readConfig, &Meter::setConfig>("Config", meter),
};
// The same two Fields in a group of the device's Model, for the encoded path.
inline constexpr ts::Model model{ts::group("meter",
    ts::field<&Meter::readLimit, &Meter::setLimit>("Limit", meter),
    ts::field<&Meter::readConfig, &Meter::setConfig>("Config", meter))};

int main() {
    assert(localFields.write<0>(240.0f) == ts::WriteResult::Applied);
    assert((localFields.readAs<double, 0>() == 240.0));
    assert(localFields.writeAs<0>(245) == ts::WriteResult::Applied);

    const auto view = localFields.read<1>();
    assert(view && view.valueOrNull() == &meter.config);
    const auto copy = localFields.readAs<Config, 1>();
    assert(copy && copy->limit == 245.0f);

    std::array<std::byte, model.maxScratch()> scratch{};
    ts::Workspace workspace{scratch};
    std::array<std::byte, ts::wireSize<Config>> bytes{};
    constexpr auto configId = ts::idOf<model, "meter/Config">();
    const auto result = model.fieldIndex().readEncoded(configId, bytes, workspace);
    assert(result.dispatch == ts::DispatchStatus::Ok);
    assert(result.written == bytes.size());
    assert(workspace.used() == 0);
}
```

`read<1>()` вище зберігає вказівник на `meter.config`. `readAs<Config, 1>()`
створює окрему копію. Якщо після цього власник змінить config, view побачить
поточний об'єкт, а copy збереже значення на момент копіювання.

Готові runnable приклади всіх форм є в
[Native.cpp](../../examples/user_guide/Native.cpp),
[Encoded.cpp](../../examples/user_guide/Encoded.cpp) і
[QuickStart.cpp](../../examples/user_guide/QuickStart.cpp).

## 2. Сигнатури getter і setter

Позначення `T` нижче означає один підтримуваний native тип:
`bool`, цілі 8/16/32/64 bit, `float`, `double`, підтримуваний scoped enum,
`std::array` або aggregate struct із підтримуваними членами. Динамічні
контейнери, pointers, reference-члени, native packed structs та `void`
як значення Field не підтримуються. Правила й resource ceilings визначає
[Type](../../lib/telemetry/type/Traits.hpp); canonical формат описано в
[WireV3](../WireV3.md).

| Callback | Підтримувана сигнатура | Результат endpoint |
| --- | --- | --- |
| Owning getter | `T get() noexcept` | `read()` повертає `std::optional<T>` |
| Borrowed getter | `const T& get() noexcept` | `read()` повертає `BorrowedValue<T>` |
| Setter by value | `WriteResult set(T value) noexcept` | Exact native запис |
| Setter by const reference | `WriteResult set(const T& value) noexcept` | Exact native запис без вимоги копіювати аргумент у setter |
| Const owner getter | `T Owner::get() const noexcept` або `const T& Owner::get() const noexcept` | Можна прив'язати const owner |
| Lvalue-qualified method | `T Owner::get() & noexcept` або сумісний `const &` метод | Викликається на живому lvalue owner |

Getter не має аргументів. Setter має один аргумент точного типу getter
і повертає саме `telemetry::WriteResult` by value. Mutable reference `T&`,
rvalue reference `T&&`, volatile payload, pointer payload, throwing callbacks,
variadic callbacks, volatile owner methods і rvalue-qualified methods
відхиляються під час компіляції. Top-level const by-value параметра не
створює окремого native типу.

`read()` не містить application validation. Getter повинен сам сформувати
потрібне значення або повернути живий const-reference. Setter повинен
перевірити application limits і виконати потрібні side effects. Наприклад,
`Applied` може означати запис config та запуск перерахунку коефіцієнтів;
generic Field цього не вирішує.

## 3. Усі форми declaration

Фабрика — `telemetry::field`. Вона створює `FieldDefinition` з виведеними
типами. У таблиці `getFree`/`setFree` — free functions потрібних сигнатур,
`owner` — живий об'єкт, `getter`/`setter` — сумісні callable objects.

| Форма | Приклад | Що зберігається |
| --- | --- | --- |
| Known free getter | `field<&getFree>("Value")` | Target у типі binding |
| Known free getter і setter | `field<&getFree, &setFree>("Value")` | Два known targets |
| Known owner getter | `field<&Meter::readLimit>("Limit", meter)` | Borrowed owner address |
| Known owner getter і setter | `field<&Meter::readLimit, &Meter::setLimit>("Limit", meter)` | Borrowed owner та два method targets |
| Owner wrapper | `field<&Meter::readLimit>("Limit", std::cref(meter))` | Borrowed underlying object |
| Rebindable owner | `field<&Meter::readLimit>("Limit", ownerSlot)` | Borrowed `OwnerSlot<Meter>` |
| Runtime function pointer | `field("Value", &getFree)` | Копія exact function pointer |
| Runtime function pointer pair | `field("Value", &getFree, &setFree)` | Копії обох pointers |
| Capture-free lambda | `field("Value", []() noexcept -> float { return 1.0f; })` | Копія перетвореного native function pointer |
| Named functor або capturing lambda | `field("Value", getter)` | Borrowed callable lvalue |
| Callable wrapper | `field("Value", std::ref(getter))` | Borrowed underlying callable |
| Getter і setter різних binding kinds | `field("Value", getter, setter)` | Кожна сторона обирає свою форму binding |
| Callable slot | `field("Value", getterSlot)` | Borrowed slot address |
| Getter/setter slots | `field("Value", getterSlot, setterSlot)` | Два independently selected targets |

Owner — actual object lvalue відповідного класу або derived object з
доступним base adjustment. Pointer та smart-pointer передайте як `*pointer`
лише після власної перевірки lifetime; pointer variable не є owner binding.
Const object допускає тільки метод, який callable на ньому. Temporary
owner, conversion proxy до owner і temporary capturing closure не можуть
створити borrowed definition.

Runtime function pointer копіюється: подальша зміна вихідної pointer variable
не змінить definition. Для динамічного target використовуйте slot.
Stateless callable має мати однозначне nothrow conversion до function pointer;
звичайний stateful callable — однозначний сумісний `operator()`. Для generic
або overloaded callable оберіть explicit typed adapter або exact slot signature.
Для overloaded method можна передати explicit cast до потрібного member pointer.

Name — non-null, nonempty UTF-8 C-string у межах string ceiling. Name storage
borrowed і має залишатися immutable. Invalid declaration name — contract
violation: constexpr construction дає diagnostic, runtime construction
завершується через `abort`, а не повертає `WriteResult`.

### Slot форми

Для owning getter виберіть `Sig = T() noexcept`; для borrowed getter —
`Sig = const T&() noexcept`. Setter slot має signature
`WriteResult(T) noexcept` або `WriteResult(const T&) noexcept`.

| Slot | Declaration і bind | Володіння |
| --- | --- | --- |
| `OwnerSlot<Meter>` | `field<&Meter::readLimit>("Limit", slot)`; `slot.bind(meter)` | Позичає owner |
| `FunctionSlot<Sig>` | `field("Value", slot)`; `slot.bind(&getFree)` | Зберігає function pointer |
| `ContextFunctionSlot<Sig>` | `field("Value", slot)`; `slot.bind(callback, context)` | Позичає context; callback отримує додатковий перший `void*` |
| `DelegateRefSlot<Sig>` | `slot.bind<&getFree>()`, `slot.bind<&Meter::readLimit>(meter)` або `slot.bind(getter)` | Позичає named callable/owner або зберігає free function target |
| `DelegateSlot<Sig, Bytes, Align>` | `slot.bind([&meter]() noexcept -> float { return meter.readLimit(); })` | Володіє closure у bounded inline storage |

Усі slots мають `bind`, `reset`, `available`, `explicit operator bool` і
`get`; callable slots також мають `invoke` з precondition engaged target.
Вони noncopyable/nonmovable. Default `DelegateSlot` capacity — 32 bytes,
alignment — `alignof(std::max_align_t)`. Closure construction, movement,
destruction та invocation повинні бути nothrow і fit у задані size/alignment;
heap fallback не використовується. Лvalue closure копіюється в owning slot,
rvalue closure переміщується. Closure ownership не подовжує lifetime її
reference captures.

Slot signature не додає numeric conversion: getter/setter value types повинні
збігатися. Bind generic/overloaded callable допускається, коли slot може
однозначно resolve його exact signature. Детальні bind overloads і правила
delegate targets — у [slot guide](../../lib/telemetry/slot/README.md).

Empty getter дає empty native read. Empty declared setter дає `Unavailable`.
Відсутній setter declaration дає `ReadOnly`. Rebinding/reset не змінює
declared writable capability чи descriptor fingerprint. Endpoint snapshots
target один раз на операцію; bind/reset має бути serialized із викликами.

## 4. FieldDefinition: native API

| Member | Тип результату | Значення |
| --- | --- | --- |
| `Value` | Native `T` | Тип getter після зняття const-reference |
| `ReadResult` | `optional<T>` або `BorrowedValue<T>` | Owning/borrowed read contract |
| `borrowsValue` | `static constexpr bool` | Getter повертає exact `const T&` |
| `writable` | `static constexpr bool` | Setter оголошено, незалежно від slot availability |
| `name()` | `const char*` | Borrowed name |
| `read()` | `ReadResult` | Один getter invocation; empty result, якщо target unavailable |
| `write(value)` | `WriteResult` | Exact `T`; argument category forwarded |
| `readAs<To>()` | `std::optional<To>` | Owning copy або checked numeric conversion |
| `readAsResult<To>()` | `FieldReadResult<To>` | Та сама owning операція з причиною відмови |
| `writeAs(value)` | `WriteResult` | Explicit conversion до `T`, потім setter |

Exact `write` допускає тільки той самий native тип після зняття cv/ref;
`int` не підміняє `float`, навіть коли звичайний C++ дозволив би implicit cast.
Для цього існує explicit `writeAs`. Const structural `T` можна передавати
by const reference; mutable volatile object відхиляється.

### BorrowedValue

| API | Значення |
| --- | --- |
| `BorrowedValue<T>{}` | Empty view |
| `BorrowedValue<T>::from(value)` | Borrow exact non-volatile `T` lvalue; temporary/proxy відхиляється |
| `hasValue()`, `explicit operator bool` | Перевірка наявності |
| `valueOrNull()` | `const T*`; safe на empty view |
| `value()`, `operator*()` | `const T&`; precondition `hasValue()` |
| `operator->()` | `const T*`; dereferencing requires engagement |

Copy view копіює лише pointer. Getter-returned object має жити й залишатися
стабільним для всіх наступних view accesses. Const-view обмежує доступ через
цей view; він не блокує owner mutations і не створює snapshot.

### Runtime borrowed read та причина відмови

`local.readBorrowed<T>(position)` і `catalogs.readBorrowed<T>(id)` повертають
`BorrowedValue<T>` тільки для declared exact `Value == T` та `borrowsValue`.
Alias того самого `T` підходить; інша структура з такими самими members — ні.
Numeric conversion і owning getter не створюють view: результат empty,
getter не викликається. Invalid position/ID та unavailable borrowed getter
також дають empty view. Dispatch містить лише функції для requested `T`:
ні копії `T`, ні Workspace, ні сховища універсального value тут немає.

Static `readBorrowed<T, Position>()` / `readBorrowed<T, Id>()` зберігає той
самий exact borrowed contract; incompatible declaration дає diagnostic.
Усі table/catalog borrowed форми вимагають lvalue table. View не продовжує
lifetime owner, slot чи getter-returned object.

`readAsResult<T>` — explicit owning convenience API з тими самими conversion
rules, що й `readAs<T>`. Runtime lookup розрізняє чотири причини відмови:

| `FieldReadStatus` | Значення |
| --- | --- |
| `Ok` | Requested owning `T` готовий |
| `NotFound` | Original-width position/ID invalid або row відсутня |
| `TypeMismatch` | Declared shape не допускає exact type або numeric conversion |
| `Unavailable` | Shape підходить, але getter binding не resolved |
| `ConversionFailed` | Getter викликано, але numeric value не representable |

Перевірки йдуть у порядку lookup → declared shape → binding → conversion.
Shape mismatch не викликає getter, навіть якщо його slot unavailable.
Successful read та failed conversion викликають getter рівно один раз.
Static `readAsResult<T, Position/Id>()` і definition `readAsResult<T>()`
перевіряють declared shape на compile time, тому runtime `NotFound` та
`TypeMismatch` у них не виникають.

```cpp
const auto view = localFields.readBorrowed<Config>(position);
if (view)
    consume(view.value()); // owner має жити до завершення consume

const auto result = localFields.readAsResult<double>(position);
if (result.hasValue())
    consume(result.value());
else
    report(result.status());
```

`FieldReadResult<T>` owns requested `T` лише при `Ok`; `hasValue()` та explicit
`operator bool` перевіряють цей стан. `value()` / `operator*` вимагають
`hasValue()`, а `valueOrNull()` повертає null на failure. `operator->` повертає
той самий owned pointer. Failure не default-constructs `T`; exact owning
success будує `T` без intermediate value container. `successFrom(factory)`
вимагає exact `T` result і будує його в final storage; `failure(status)`
вимагає один із чотирьох failure statuses. Result wrapper має storage для `T`,
тому для великого payload без owning copy обирайте `readBorrowed<T>`.
Ці native statuses не є wire codes і не змінюють encoded dispatch.

Pointer/reference із `FieldReadResult<T>` позичає **його власний** payload.
Тримайте named result живим до останнього використання pointer:

```cpp
const auto result = localFields.readAsResult<double>(position);
if (const double* value = result.valueOrNull())
    consume(*value); // result усе ще живий.
```

`valueOrNull()` та `operator->()` доступні лише для lvalue result: `&` повертає
`T*`, `const&` — `const T*`. Їхні `&&` / `const&&` overloads явно видалені.
Тому `localFields.readAsResult<Config>(position)->limit` не компілюється,
навіть якщо цей immediate-use expression раніше вкладався в lifetime result.
Рекомендована форма з перевіркою failure:

```cpp
const auto result = localFields.readAsResult<Config>(position);
if (result)
    consume(result->limit); // result усе ще живий.
```

Це лише compile-time обмеження pointer access, без runtime checks та зміни
storage/layout. `value()` і `operator*()` зберігають свої return categories:
lvalue дає `T&` / `const T&`, mutable rvalue — `T&&`, const rvalue використовує
наявний `const&` overload і дає `const T&`. Для відомого successful result
immediate-use `factory().value().member` або `(*factory()).member` лишається
допустимим. Для move-out збережіть non-const result і створіть власний object
через `auto value = std::move(result).value();`; збережена reference сама не подовжує
lifetime payload. Це відрізняється від `BorrowedValue<T>`: там сам T належить
зовнішньому owner, тому pointer access на temporary view лишається допустимим,
доки backing object живий.

### Правила `readAs` / `writeAs`

| Conversion | Поведінка |
| --- | --- |
| Exact structural `T` | `readAs<T>` створює owning copy; `writeAs(T)` передає exact value |
| Різні structural types | Compile-time selected API дає diagnostic; runtime selected API повертає empty read / `InvalidValue` без getter/setter |
| Integer → integer | Range/sign перевірено в integer representation; overflow і negative-to-unsigned відхиляються |
| Numeric → bool | Zero → false, nonzero → true; floating NaN/infinity відхиляються |
| Integer → float/double | Діапазон підтримуваних integers поміщається; precision може округлюватися |
| Float/double → integer | Finite representable truncated value; fraction truncates toward zero, NaN/infinity/overflow відхиляються |
| Float → double | Значення, NaN та infinity переносяться |
| Double → float | Finite overflow відхиляється; rounding/underflow можливі; NaN/infinity зберігають відповідну категорію |
| Enum ↔ numeric / enum | Conversion через exact underlying integer; dictionary membership не є validation |

Наприклад, `12.75` може бути перетворено на integer `12`. Для unsigned
destination дробове `-0.5` може truncation дати `0`; це representation rule,
а не дозвіл застосунку на негативну уставку. Якщо така політика небажана,
перевірте її до `writeAs` або в окремій прикладній команді.

Numeric conversion не гарантує збереження кожного біта precision. Exact
native read/write зберігає declared type. Непозначений, але representable
enum code може пройти codec/conversion; setter має сам перевірити дозволені
режими. Fast-math, finite-math-only та `/fp:fast` для checked conversion
не підтримуються.

Failed readAs не пояснює причину: unavailable target, invalid runtime ID,
structural mismatch і range failure дають `nullopt`. Failed numeric writeAs
дає `InvalidValue` до setter. Runtime selection відсутнього Field дає
`NotFound`; read-only declaration дає `ReadOnly` до conversion/callback.
Для причини read failure використовуйте `readAsResult`; contract `readAs`
залишається `optional<To>`.

## 5. Local table, catalogs та IDs

`FieldTable` володіє declarations і erased rows, але не owners/values.
Local position — declaration order від zero; local API приймає integer або
scoped position enum. `model.fields()` — catalogs моделі (internal engine під
деревом, по рядку на групу); catalog size — кількість groups, не Fields.

Global `PackedId` має high 16 bits group і low 16 bits local position:
`ts::idOf<model, "meter/Config">()` дає другий Field групи `meter`. Порядок
declarations визначає identity; names не беруть участі в runtime lookup.

Модель із дерева груп (`Model{adc::tlm::branch, ...}`) дає той самий доступ
за шляхом замість позицій: `telemetry::endpoint<model, "adc/channel1/Gain">`
оголошує handle (`gain1.read()`, `gain1.write(2.5f)`), а
`telemetry::read<model, "adc/Config.rateHz">()` читає разово. Шлях
розв'язується під час компіляції в той самий PackedId і не змінюється від
перестановки оголошень; помилковий шлях — build error. Дивіться
[Model: group tree](Model.md#group-tree-модель-із-файлів-гілок).

| API | Тип/результат | Призначення |
| --- | --- | --- |
| `local.get<Position>()` | `const FieldDefinition&` | Exact declaration; invalid static position дає diagnostic |
| `local.read<Position>()` | Definition `ReadResult` | Exact native read |
| `local.write<Position>(value)` | `WriteResult` | Exact native write |
| `local.readAs<To, Position>()` | `optional<To>` | Static selection, explicit conversion |
| `local.readBorrowed<T, Position>()` | `BorrowedValue<T>` | Static exact borrowed declaration |
| `local.readAsResult<To, Position>()` | `FieldReadResult<To>` | Static owning read з native status |
| `local.writeAs<Position>(value)` | `WriteResult` | Static selection, explicit conversion |
| `local.readAs<To>(position)` | `optional<To>` | Checked runtime local selection |
| `local.readBorrowed<T>(position)` | `BorrowedValue<T>` | Checked exact borrowed runtime read |
| `local.readAsResult<To>(position)` | `FieldReadResult<To>` | Checked owning read з причиною відмови |
| `local.writeAs(position, value)` | `WriteResult` | Checked runtime local selection |
| `model.fields().readAs<To>(id)`, `writeAs(id, value)` | Owning read / `WriteResult` | Checked runtime global conversion access |
| `model.fields().readBorrowed<T>(id)` | `BorrowedValue<T>` | Checked runtime exact borrowed read |
| `model.fields().readAsResult<To>(id)` | `FieldReadResult<To>` | Checked runtime owning read з native status |
| `size()`, `empty()` | `size_t`, `bool` | Entry count for local table; group count for catalog table |
| `data()`, `begin()`, `end()` | Borrowed row pointers | Erased iteration, без getter invocation |
| `operator[](i)` | `const FieldEntry&` або `const FieldCatalog&` | Unchecked; caller requires `i < size()` |
| `forEach(visitor)` | `void` | Typed declaration traversal |
| `forEachWhile(visitor)` | `bool` | Typed traversal до exact-`bool` false; true означає повний обхід |
| `model.fields().forEachEntry(visitor)` | `void` | Packed ID, group name та erased FieldEntry |
| `model.fields().forEachEntryWhile(visitor)` | `bool` | Такий самий erased обхід до exact-`bool` false |
| `visit(position_or_id, visitor)` | `bool` | Checked typed selection; callback return ignored |
| `model.fieldIndex()` | `FieldIndex` | Copyable borrowed runtime view |

Typed доступу за compile-time packed ID (`read<Id>()`, `get<Id>()`) немає: його
дає шлях або handle. `FieldTable` не має `index()`: для local encoded row
використовуйте `local[position].readEncoded/writeEncoded`; для checked global
lookup — `model.fieldIndex()`. Runtime packed IDs приймають integral values, не local
enum. Вони перевіряються до narrowing: negative/wide invalid ID не може
wrap до іншого валідного endpoint. ID відомого endpoint дає
`ts::idOf<model, "path">()` або handle `.id`; публічного `makeId` немає.
`tryGroupOf`/`tryIndexOf` повертають optional components; `groupOf`/`indexOf`
вимагають valid u32 packed input. Component validity ще не доводить існування
row у конкретній Model.

Local `forEach` приймає ordinary `visitor(definition)` або, якщо callable
підтримує її, форму `visitor.template operator()<Position>(definition)`.
Global `forEach` передає `catalogName, definition` звичайному callback або
`operator()<Group, Entry>(catalogName, definition)` typed callback.
Local та global `visit` обидва викликають тільки `visitor(definition)`:
catalog name й compile-time position parameters там не передаються.
Callback має компілюватися для кожного можливого definition. Для різних
request/value types використовуйте `if constexpr`. Traversal callbacks можуть
кидати exceptions, якщо вони enabled; endpoint callbacks залишаються noexcept.
`forEachWhile` має ті самі typed visitor forms, але вимагає exact `bool`.
`forEachEntry` дає metadata, не live values. Empty обхід завершується true;
false від останнього visitor також означає requested stop. Повні signatures
та приклади — [Tables and catalogs](TablesAndCatalogs.md#обійти-definitions-або-metadata).

Публічний `RootTypes` містить один value type на кожен Field у порядку
оголошення; повтори навмисні. `RootTypes::size` — кількість полів, тому
`ValuesFile` залишає окремий token для кожного поля навіть із тим самим типом.
`RegistryRootTypes` містить лише унікальні типи в порядку першого входження:
це окремий compile-time вхід для Registry, а не список рядків таблиці.
Каталог має обидва aliases й зберігає порядок груп та полів усередині груп.
`staticSize` — кількість рядків локальної таблиці або груп у каталозі;
`TypeStorage<Registry>` зберігає positional TypeIds для всіх рядків.
`Model::typeId<T>()`,
`model.view().fieldTypeId(id)` і `model.types()` дозволяють звірити native
тип з registry. TypeId є identity типу в конкретному complete Model,
а packed Field ID — identity endpoint; це різні простори.

## 6. Encoded runtime API

Encoded boundary отримує лише canonical value payload, caller-owned byte
spans і `Workspace&`. Framing, operation code, peer, Bind, request correlation
та retry policy залишаються в transport. Повний шлях показано в
[TransportWalkthrough](TransportWalkthrough.md); byte layout — у
[WireV3](../WireV3.md).

| API | Результат | Значення |
| --- | --- | --- |
| `FieldEntry::readEncoded(output, workspace)` | `EncodedReadResult` | Checked read однієї erased row |
| `FieldEntry::writeEncoded(input, workspace)` | `EncodedWriteResult` | Checked write однієї erased row |
| `FieldIndex::find(id)` | `const FieldEntry*` | `nullptr`, якщо ID invalid/absent |
| `FieldIndex::readEncoded(id, output, workspace)` | `EncodedReadResult` | Lookup + checked read |
| `FieldIndex::writeEncoded(id, input, workspace)` | `EncodedWriteResult` | Lookup + checked write |
| `FieldIndex::catalogs()`, `count()` | Catalog pointer, `uint32_t` | Borrowed catalog array та group count |
| `readFieldEncoded(modelView, id, output, workspace)` | `EncodedReadResult` | Compiled ModelView adapter |
| `writeFieldEncoded(modelView, id, input, workspace)` | `EncodedWriteResult` | Compiled ModelView adapter |

`FieldEntry` містить `name`, `wireBytes`, `readScratchBytes`,
`writeScratchBytes`, contexts і raw operation pointers. Public raw pointers
служать ABI/implementation boundary: користуйтеся checked methods, а не
викликайте `read`/`write` без length/overlap preflight. Index, створений
вручну з pointers/count, вимагає coherent live rows; `find` перевіряє ID,
а не достовірність forged backing pointers.

Read output має вмістити щонайменше `wireBytes`; writes вимагають input length
точно `wireBytes`. Read-only write повертає successful dispatch з `ReadOnly`
до input decoding. Encoded bool допускає лише byte 0/1; malformed
representation не викликає setter. Borrowed getter не потребує owning value
storage: `readScratchBytes == 0`; setter тієї самої великої структури може
потребувати Workspace, тому read/write bounds незалежні.

Коли операції потрібен scratch, payload span не може overlap Workspace
storage. Borrowed getter encoding також вимагає, щоб записуваний payload був
disjoint з native object, який getter повернув; alias refusal може виникнути
після getter invocation. Тримайте buffers disjoint з application objects.
`model.maxFieldScratch()` дає sufficient fresh Workspace capacity для одного
Field read/write, включаючи alignment margin. Усі lease lifetimes LIFO;
nested/concurrent операції потребують окремого Workspace або бюджету всіх
одночасно живих leases. Деталі — [Workspace](../../lib/telemetry/codec/Workspace.hpp).

### Результати та помилки

| `WriteResult` | Code | Meaning |
| --- | --- | --- |
| `Applied` | 0 | Setter застосував зміну |
| `NotFound` | 1 | Runtime native selection не знайшов Field; callback також може повернути цей supported status |
| `ReadOnly` | 2 | Setter не оголошено або callback відмовив з цим status |
| `InvalidValue` | 3 | Conversion або application validation відхилила value |
| `Busy` | 4 | Application owner зараз не приймає запис |
| `Unavailable` | 5 | Declared late-bound setter/owner відсутній або application повернула status |

| `DispatchStatus` | Code | Encoded meaning |
| --- | --- | --- |
| `Ok` | 0 | Read payload готовий або write endpoint status доставлений |
| `NotFound` | 4 | Index lookup не знайшов row |
| `InvalidPayload` | 5 | Length/representation/overlap refusal |
| `BufferTooSmall` | 6 | Read output недостатній |
| `WorkspaceTooSmall` | 7 | Немає достатнього aligned scratch |
| `InternalError` | 8 | Setter повернув невизначений status code |
| `Unavailable` | 9 | Getter/setter/owner binding не resolved |

`EncodedReadResult` має `dispatch` і `written`. При `Ok` written дорівнює
value wire size; лише цей prefix можна використати. `EncodedWriteResult` має
`dispatch` та `endpointStatus`; application status meaningful тільки при
`dispatch == Ok`. `Ok + InvalidValue` означає, що setter виконано й він
відмовив; `InvalidPayload` означає відмову encoded boundary. Native calls
повертають callback status напряму; encoded writes додатково відхиляють
unknown status через `InternalError`.

## 7. Lifetime, threads і прикладні гарантії

Local та catalog tables noncopyable/nonmovable: erased contexts можуть
посилатися на їхні власні declarations. Row pointers, `get`, traversal і
index borrowing APIs require lvalue table. Owners, callable lvalues,
names, slots і tables мають жити за stable addresses до останнього use.
Model/index/view копіює pointers, не objects і не values.

Serialize owner updates, telemetry access та slot bind/reset/destruction.
Жодних locks, автоматичних atomic snapshots або cross-Field transaction
у Field немає. Getter-returned `T` є snapshot одного getter; кілька read
викликів можуть належати різним інстантам. Borrowed result вимагає ще й
стабільного owner object до завершення використання reference. Owned callback
не повинен reset/replace власний slot під час виконання.

Setter отримує валідне native представлення, але не автоматично валідну
уставку. Перевірте діапазони, enabled/disabled policy, допустимі enum codes
та узгодженість членів до першої зміни state. Application failures не
відкочують уже виконані side effects. Framing віддає повний payload один
раз; повторний write повторно викликає setter.

## 8. Імена типів у descriptor

Descriptor v3.1 називає кожен enum і struct, які в ньому є, а clients
показують ці імена: `struct adc::Config` замість `struct (3)`, `enum adc::Mode`
замість `enum`. Нічого писати не треба: ім'я береться з коду.

| Оголошення | Ім'я в descriptor |
| --- | --- |
| `namespace adc { struct Config; }` | `adc::Config` |
| `class Adc { struct Config; };` (вкладений клас) | `Adc::Config` |
| `namespace power::adc { enum class Mode; }` | `adc::Mode`: лише найближчий scope |
| `struct Config;` у global або anonymous namespace | `Config` |
| `Spectrum<4>`, спеціалізація шаблону | без імені |
| Локальний клас у функції | build error: потрібне явне ім'я |

- Ім'я — найближчий scope і власне ім'я типу, один рівень. Тому `adc::Config`
  і `metrology::Config` із різних гілок дерева не конфліктують. Масиви,
  scalars і void імен не мають.
- Імена унікальні в одному descriptor. Два типи з однаковим ім'ям — build
  error у constexpr descriptor (`duplicateTypeName()`), а runtime descriptor
  повертає `DescriptorError::DuplicateName`.
- Ім'я, автоматичне чи явне, має форму `[scope "::"] name`: щонайбільше одне
  `::`, обидві частини непорожні, без `:`, `/`, `.`, `[`, `]` і `\`. Частина
  має не більше `maxNameBytes` (64) байтів profile.

### Override: TypeReflection

Явне ім'я замінює автоматичне, `noTypeName()` прибирає його. Так розв'язують
збіг імен, дають ім'я спеціалізації шаблону чи ховають внутрішній тип:

```cpp
template<>
struct telemetry::reflection::TypeReflection<metrology::Spectrum<4>> {
    static constexpr auto name = telemetry::reflection::typeName("metrology::Spectrum4");
};

template<>
struct telemetry::reflection::TypeReflection<app::Internal> {
    static constexpr auto name = telemetry::reflection::noTypeName();
};
```

Як і `EnumReflection`, specialization має стояти перед першим descriptor, що
містить тип, у кожному translation unit; пізня specialization — ODR-помилка,
яку compiler може не діагностувати. У моделі з файлів гілок вона живе у файлі
гілки, якій належить тип, перед її group expression. `EnumReflection` і
`TypeReflection` незалежні: перший задає імена enumerators, другий — ім'я
самого enum.

### Вимкнути імена: TELEMETRY_TYPE_NAMES 0

Перемикач проєкту живе у власному `telemetry_config.hpp` проєкту, який SDK
знаходить на include path, як `lwipopts.h` чи `FreeRTOSConfig.h`; `-D` теж
працює:

```cpp
// telemetry_config.hpp
#define TELEMETRY_TYPE_NAMES 0
```

З 0 жоден рядок імені не потрапляє в image; descriptor дерева груп лишається
v3.1, бо несе GroupTree flag. Ім'я коштує 4 байти довжини плюс свої байти: у
[NUCLEO demo](../../examples/nucleo_h7s3l8/README.md#type-names) чотири імена
займають 90 B packed descriptor. Значення має бути однаковим у кожному
translation unit; compiled SDK sources перетворюють розбіжність на link error.
Hosts читають імена завжди, перемикач вирішує лише, що надсилає пристрій.

Hosts, зібрані до descriptor v3.1, відмовляють пристрою з іменами. Тоді або
перезберіть host, або перегенеруйте static SDK (код користувача компілюється
без змін), або поставте `TELEMETRY_TYPE_NAMES 0`. Для однієї програми, що
має віддавати обидві форми, є profile `descriptor::withoutTypeNames(model)`.
Байти й правила — [Descriptor і Values](DescriptorAndValues.md).

## 9. Джерела й подальші приклади

- [FieldDefinition та factory overloads](../../lib/telemetry/field/Field.hpp).
- [FieldTable та erased FieldEntry](../../lib/telemetry/field/FieldTable.hpp).
- [FieldCatalogTable та FieldIndex](../../lib/telemetry/field/FieldCatalogs.hpp).
- [BorrowedValue](../../lib/telemetry/result/BorrowedValue.hpp),
  [FieldReadResult](../../lib/telemetry/result/FieldReadResult.hpp),
  [checked numeric conversions](../../lib/telemetry/detail/NumberConversion.hpp).
- [Native.cpp](../../examples/user_guide/Native.cpp): slots, exact values,
  borrowed Settings/Block, static/runtime As, iteration і IDs.
- [Encoded.cpp](../../examples/user_guide/Encoded.cpp): preflight та callback
  counters, read-only write, malformed bool і large borrowed payload.
- [Device integration](../../examples/device_integration/README.md): private
  bindings за звичайним runtime application facade.
- [Group tree](../../examples/group_tree/README.md): модель із файлів гілок,
  named handles, доступ за шляхом і override імені типу.
- [Commands](Commands.md) для action/status та [Services](Services.md)
  для request/response операцій.

Authors: Ruslan Kovtun (shpegun60), codexAi. [MIT](../../LICENSE).

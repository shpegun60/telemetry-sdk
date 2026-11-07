# Qt/QML scalar Watchlist

This host behavior requires the corresponding sequential-Watchlist revision of
Telemetry UX; older UI versions retain a 32-row list limit. The portable SDK
still limits each individual ReadMany wire operation to 32 items.

The optional Watchlist reads selected independent scalar or open-enum Fields
from the existing Qt/QML Workbench. The host list is bounded by the current
Descriptor and Qt row/storage capacity, rather than a 32-Field list limit.
Each ReadMany operation remains bounded to at most 32 IDs and may be smaller
according to the discovered item, request-byte and worst-case reply-byte limits.
Longer lists run as ordered sequential chunks through the existing FIFO.
Struct members and array elements are not independent Field IDs. Neither a
single batch nor a complete host cycle promises a simultaneous snapshot.

## Use

1. Start the [Qt/QML application](https://github.com/shpegun60/telemetry-ux/blob/main/apps/inspector/README.md).
   **ReadMany demo** supplies 32 scalar Fields without hardware.
2. For the optional NUCLEO UART ReadMany firmware, enable **Discover ReadMany on
   the next UART connection** before connecting. Discovery obtains the existing
   Descriptor and TRM1 Resource record. Legacy UART discovery remains unchanged
   when this option is off. No firmware is programmed by the GUI.
3. Select a scalar Field in the endpoint tree and press **Add selected**.
   Add distinct readable scalar Fields from other catalogs in the same way.
4. Choose **ReadMany** and press **Read once** or **Start polling**. The interval
   control accepts whole milliseconds from 1 to 60,000. This is a nominal
   scheduling interval, not a throughput guarantee: UART airtime, complete cycle
   duration, getter execution and owner scheduling determine the actual rate.
   Missed ticks are skipped; a slow cycle never accumulates more requests.
5. For a device without TRM1, explicitly choose **Individual FieldRead**. A cycle
   then reads each selected Field sequentially. There is no automatic fan-out
   or substitution after a failed batch.

Only one Watchlist read is pending. The same Workbench-owned DynamicClient and
FIFO execute foreground Field/Command/Service operations and Watchlist reads.
An active foreground operation delays Watchlist submission. There is no second
transport, worker thread or queue. Watchlist reads preserve the foreground
snapshot, editor draft and Service response.

## Lifetime and results

The completion copies formatted scalar values and per-item statuses into
Qt-owned strings before leaving the callback. U64/S64 never pass through a QML
Number. Unknown enum values retain their signed numeric code; NaN and negative
zero display their wire bits. Failed items replace their previous displayed
value with an explicit status.

Stop prevents subsequent reads and lets an already accepted read drain. A
superseded completion is ignored. Editing the list or changing its mode waits
for the entire logical cycle, including an Individual cycle paused behind a
foreground operation, and for any accepted read to drain after Stop. List,
mode and interval controls remain disabled during that interval; Stop stays
available. Disconnect, schema invalidation or a foreground transport failure
stops polling and notifies existing QML rows that their values are stale.
Each fully validated successful chunk publishes its own rows. Stop retains an
already published prefix, ignores an outstanding superseded completion and
prevents later chunks. A failed cycle invalidates its displayed values.
Logical invalidation does not discard an outstanding read's completion.
The next admission starts an empty list. A timed-out prepared UART
exchange still requires the existing session-recovery procedure; the Watchlist
does not retry it automatically.

TRM1 session and Descriptor fingerprint are checked before admission. When an
old TRM1 session needs recovery, discovery from the same UART peer must prove
a strictly newer remote session before the previous session is retired.

## Optional portable bridge

`Telemetry::read_many_workbench` installs
[`integration/read_many/Workbench.hpp`](../lib/integration/read_many/Workbench.hpp).
It depends only on `workbench` and `read_many_client`. Its `bindReadMany()`,
`readMany()` and `readFieldValue()` functions use the private existing client
without changing Workbench object layout or exposing arbitrary encoded calls.
The ordinary `workbench` component has no ReadMany wire dependency.

The GUI explicitly selects a larger callback budget to own the bounded batch
binding. This does not increase the portable Workbench default. Qt-owned row
storage and display strings belong to the application, not the portable core.

## Qualification scope

Maintained portable bridge checks cover FIFO sharing, preserved drafts,
reentrancy, allocation traps and lifecycle failures. Qt tests exercise actual
production Encoded dispatch, bounded scheduling, owned results, exact scalar
display and the QML controls. Regression tests inspect already-rendered QML
rows after FieldRead, Apply, Command and Service failures, including a lost
Apply readback. They also pause Individual cycles behind foreground work and
verify unchanged IDs, mode, interval and remaining read order. Deterministic
host checks also cover 65 distinct Fields as 32/32/1 ReadMany packets and as 65
explicit Individual reads, prepared-read Stop/drain, and a malformed middle
chunk. The shipped legacy and 32-scalar demonstration Descriptors are unchanged;
the larger test Model is an explicit opt-in profile. UART integration uses the established transport
and opt-in TRM1 bootstrap. This feature does not claim an additional physical
board run or sustained DMA/cache stress qualification.

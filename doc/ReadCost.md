# What a read costs on the wire

The owner asked on 14 November 2026 whether the Watchlist and the Device tree
read the device in the best way the protocol allows, and whether the protocol
itself could do better. This note answers before the tree's previews are
reworked (PV-2 in the Client roadmap). It compares the three read paths that
exist today - FieldRead, ReadMany and a Batch of FieldReads - for the Watchlist
and for a snapshot of the tree, then weighs what a protocol change could add.

## How the numbers were obtained

- **Bytes are computed from the wire formats.** The message bodies are those
  of [ReadMany](ReadManyEncoded.md) and [Batch](MixedBatchEncoded.md), which
  the wire code implements exactly. Serial framing is counted as the encoder
  sends it: a 2-byte length, the body and a CRC16 in COBS, then the delimiter,
  6 bytes a frame. COBS adds a further byte only after 254 non-zero bytes in a
  row, which a value of 254 bytes or more may have; such values are rare here.
- **The model is checked against measurements.** Eight FieldReads of 4-byte
  scalars take 544 protocol and 640 framed bytes in
  [Read comparison](ReadComparison.md); the model gives the same. On the
  NUCLEO over its ST-LINK serial bridge at 115 200 baud, ReadMany of 8 and 32
  Fields took 29.2 and 60.8 ms on average over 30 minutes
  ([evidence](evidence/read-many-uart-load-2026-11-02/README.md)), and 32
  FieldReads took 540 ms once ([evidence](evidence/read-many-uart-2026-11-01/README.md)).
  Their airtime is 18, 51 and 222 ms: each exchange costs about **10 ms
  beyond its airtime** (9.5-11.3 ms on average, 6-17 ms at the extremes). The
  model below adds those 10 ms an exchange and reproduces all three: 28, 61
  and 542 ms.
- **Who owns those 10 ms is not established.** The device's own work is
  small by the code (an O(1) lookup and one getter call an item; an encoded
  4 KiB read measured at 37k-68k cycles, while one serial byte lasts about
  52k); the inference is that the USB serial bridge and the host take most of
  it. A logic analyser on the UART would settle it (see "Still to measure").
- **TCP is not measured.** "Bytes on TCP" are TEQ1 bytes only: the SDK's TCP
  framing adds none, but every exchange also costs TCP/IP headers and
  acknowledgements, and its turnaround on the demo device is unknown.
- **Default limits.** Each device advertises its own: the model uses
  ReadMany's 32 items with a 514-byte reply body, a Batch of 32 items within
  the demo device's 4096/4096 bytes, and the serial link's 2048-byte frame body
  (a reply body of at most 2016 bytes). A device that advertises less gets
  smaller portions and more exchanges.

Only one request is in flight at a time (one FIFO), so a cycle takes its
airtime plus one turnaround **per exchange**. On the serial link that
turnaround is worth about 115 bytes of airtime; on TCP, where airtime is small,
the exchange count is nearly all of it.

## Cost of one exchange

| Path | Request | Reply | Items an exchange |
| --- | --- | --- | --- |
| FieldRead | 32 | 32 + value | 1, any type |
| ReadMany | 34 + 4 an item | 34 + (8 + value) an item | up to 32 scalars or enums |
| Batch of FieldReads | 34 + 8 an item | 34 + (12 + value) an item | up to 32, any type, within the byte budgets |

An item costs 64 bytes plus its value and a whole exchange as a FieldRead, 12
bytes plus its value in ReadMany, and 20 bytes plus its value in a Batch. A
1-byte bool still carries the 12; 8-byte values make 1024 of them need 2.0 s
of airtime instead of 1.6 s.

## The Watchlist: N scalars of 4 bytes

Cycle times are for the serial link at 115 200 baud: airtime plus 10 ms an
exchange, as measured above.

| N | Path | Exchanges | Bytes on TCP | Bytes on serial | Airtime ms | Cycle ms |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 32 | FieldRead each | 32 | 2 176 | 2 560 | 222 | 542 |
| 32 | ReadMany | 1 | 580 | 592 | 51 | 61 |
| 32 | Batch | 1 | 836 | 848 | 74 | 84 |
| 256 | FieldRead each | 256 | 17 408 | 20 480 | 1 778 | 4 338 |
| 256 | ReadMany | 8 | 4 640 | 4 736 | 411 | 491 |
| 256 | Batch | 8 | 6 688 | 6 784 | 589 | 669 |
| 1024 | FieldRead each | 1 024 | 69 632 | 81 920 | 7 111 | 17 351 |
| 1024 | ReadMany | 32 | 18 560 | 18 944 | 1 644 | 1 964 |
| 1024 | Batch | 32 | 26 752 | 27 136 | 2 356 | 2 676 |

ReadMany, which the Watchlist uses for scalars, is the cheapest path the
protocol has: a quarter of FieldRead's bytes and a thirty-second of its
exchanges, about 30 % fewer bytes than a Batch. At 1024 values a cycle takes
about 2 s at 115 200 baud: with this protocol a 1 s interval cannot be kept
there, and the Watchlist then reads every 2 s while it still shows "Every 1 s".

## Struct and array Fields

ReadMany takes scalars only. The Watchlist reads a struct whose values it
watches with one FieldRead of the whole Field, one exchange a struct.

| Structs | Path | Exchanges | Bytes on TCP | Bytes on serial | Airtime ms | Cycle ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 4 of 64 B | FieldRead each (today) | 4 | 512 | 560 | 49 | 89 |
| 4 of 64 B | One Batch | 1 | 404 | 416 | 36 | 46 |
| 16 of 64 B | FieldRead each (today) | 16 | 2 048 | 2 240 | 194 | 354 |
| 16 of 64 B | One Batch | 1 | 1 412 | 1 424 | 124 | 134 |
| 8 of 256 B | FieldRead each (today) | 8 | 2 560 | 2 656 | 231 | 311 |
| 8 of 256 B | Batch | 1 (2 on serial) | 2 276 | 2 368 | 206 | 226 |

A Batch of the structs' FieldReads saves 44 bytes for every struct (56 framed
on the serial link), less one Batch envelope of 68 bytes (80), and one
exchange for every struct after the first. The bytes saved stay the same as
the structs grow, so their share shrinks; the exchanges saved do not. A Field
whose value is over 2016 bytes cannot be read over the serial link at all, by
any path.

Note that the serial rows for a Batch describe no device yet: the NUCLEO
firmware advertises no Batch. The demo device does, over TCP.

## A snapshot of the Device tree

For a view of F Fields, 80 % of them 4-byte scalars and 20 % 64-byte structs:

| F | Path | Exchanges | Bytes on TCP | Bytes on serial | Airtime ms | Cycle ms |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 30 | FieldRead each | 30 | 2 400 | 2 760 | 240 | 540 |
| 30 | ReadMany + FieldRead each struct | 7 | 1 220 | 1 304 | 113 | 183 |
| 30 | ReadMany + one Batch of the structs | 2 | 1 024 | 1 048 | 91 | 111 |
| 30 | One Batch of everything | 1 | 1 148 | 1 160 | 101 | 111 |
| 64 | FieldRead each | 64 | 5 132 | 5 900 | 512 | 1 152 |
| 64 | ReadMany + FieldRead each struct | 15 | 2 616 | 2 796 | 243 | 393 |
| 64 | ReadMany + one Batch of the structs | 3 | 2 112 | 2 148 | 186 | 216 |
| 64 | One Batch of everything | 2 | 2 452 | 2 476 | 215 | 235 |

The previews today are slower than the first row says: they read one Field
each 500 ms, so 30 Fields take 15 s. The wire is not what limits them.
ReadMany plus one Batch is the cheapest in bytes and, at 10 ms a turnaround,
as fast as one Batch of everything; with a longer turnaround (TCP's is
unknown) one Batch of everything would win.

## Findings

1. **The choice for scalars is already the best the protocol offers.**
   ReadMany beats a Batch by 8 bytes an item and FieldRead by an exchange an
   item. Nothing to change.
2. **The Watchlist's structs should go in one Batch where the device has
   Batch.** Today each struct is its own FieldRead; one Batch saves an
   exchange per struct after the first - 10 ms each on the measured serial
   link. Where the device has no Batch (the NUCLEO), FieldRead stays. The
   Watchlist cannot do it yet: its source has no Batch entry, a struct read is
   matched to one Field, its frames know only ReadMany and FieldRead, and its
   planner would need the Batch and transport byte budgets, keeping FieldRead
   for a struct too large for them (Client roadmap WL-6).
3. **Keep scalars in ReadMany, even beside a Batch.** On bytes and time alone,
   folding k scalars into a Batch that is sent anyway pays while one saved
   exchange outweighs 8 bytes a scalar: up to about 24 scalars at 115 200 baud
   with the measured turnaround, and nearly always on TCP. A device without
   Batch could not read the scalars at all. Since 15 November 2026 a Batch of
   FieldReads only is a read in the native client: lost after its handoff it
   times out or is cancelled as a read, not "outcome unknown" (before, it was
   counted as mutating); the session still needs its recovery either way, as
   after any lost read.
4. **The tree's previews: ReadMany plus one Batch** (PV-2), lowest in
   priority. A view of 64 Fields falls from 64 exchanges to 3, and from 32 s
   at today's pace to about 0.2 s of wire time.
5. **1024 values do not fit a 1 s cycle on a 115 200-baud link.** That is a
   matter of bytes and turnarounds, not of the client. The Watchlist should
   say its cycle time and the pace it really achieves (it does not yet; WL-6).
6. **A device on TCP should send its replies without delay.** The SDK leaves
   socket options to the application. The demo device turns Nagle's algorithm
   off on its socket; firmware that keeps it on can hold the second segment
   of a reply longer than one segment (536 bytes with lwIP's default) until
   the host's delayed acknowledgement, tens of milliseconds. A ReadMany of 32
   eight-byte values (546 bytes of reply) crosses that line. Inferred, not
   measured with TEQ1.

ValuesFile and an Aggregate Field, compared in [Read comparison](ReadComparison.md),
do not fit a list the user chooses: ValuesFile reads every scalar Field in
Descriptor order, so a sparse selection pays for the rest; an Aggregate is a
struct the application publishes at design time. An Aggregate remains the
way to get values taken at one instant; no path here gives that - one Batch
only narrows the gap between its items to one dispatch.

## What the load run already says about the 10 ms

The 30-minute ReadMany run on the NUCLEO
([evidence](evidence/read-many-uart-load-2026-11-02/README.md)) gives four
mean exchange times. A straight line through them fits within 0.25 ms:

| Fields | Mean ms | Framed bytes | Airtime ms | Beyond airtime ms |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 29.19 | 208 | 18.06 | 11.13 |
| 12 | 34.08 | 272 | 23.61 | 10.47 |
| 16 | 39.69 | 336 | 29.17 | 10.52 |
| 32 | 60.78 | 592 | 51.39 | 9.39 |

latency ≈ 18.5 ms + 1.32 ms a Field. Each Field adds 16 framed bytes, 1.39 ms
of airtime at 115 200 baud, so **the per-item cost is the wire's** - the
device's work per item does not show. The fixed 18.5 ms is 80 framed bytes
(6.9 ms of airtime) and **about 11.5 ms that is not airtime**. That part
shrinks a little as replies grow (11.1 ms at 8 Fields, 9.4 ms at 32), as if
some of it overlapped the transmission - a USB bridge passing bytes on in
packets would do that - but which of the device, the ST-LINK bridge and the
host owns it is still not established; it takes the measurement under
"Still to measure". Some items of the run were Unavailable and carried no
value, which lowers the slope slightly.

## ReadMany of any type, worked out

The table of options above first judged this from a device with Batch. On
the NUCLEO, which has none, every struct is a FieldRead of its own, so the
same model gives (serial at 115 200 baud, 10 ms a turnaround):

| What is read | Today | One Batch | One ReadMany of any type |
| --- | ---: | ---: | ---: |
| 16 structs of 64 B, device with Batch | 354 ms, 16 exchanges | 134 ms, 1 412 B | 122 ms, 1 284 B |
| The same, NUCLEO (no Batch) | 354 ms | - | 122 ms |
| A Device-tree view of 64 Fields (51 scalars, 13 structs of 64 B), with Batch | 216 ms (ReadMany + one Batch), 2 112 B | 235 ms, 2 452 B | 190 ms, 1 940 B, 2 exchanges |
| The same, NUCLEO | 393 ms (ReadMany + FieldRead each struct), 15 exchanges | - | 190 ms |

With Batch the gain is 8 bytes an item, about a tenth. Without Batch it is
two to three times, but only with a larger reply budget: these replies
(1 186 and 1 198 bytes) exceed ReadMany's 514-byte reply body, and at 514
the structs would need three exchanges, not one. A device that can spare
that buffer can as well spare Batch's.

The cheaper path to most of it is no wire change at all: Batch on the
NUCLEO's image (134 ms against 122 ms above), and a client that treats a
Batch made only of reads as not mutating, so a read that times out is not
reported as an unknown outcome (finding 3). Decided on 14 November 2026:
ReadMany stays for scalars and enums; structs go by Batch where the device
has it (Client roadmap WL-6, PV-2).

## What a protocol change could add

Each of these would be an SDK change of its own, for the owner to decide. None
is recommended before the turnaround is measured on hardware and on TCP.

| Option | What it saves | What it costs |
| --- | --- | --- |
| **Device-pushed values** (a subscription), or only the values **changed since** the last | Every request and the wait between exchanges, not the latency. Largest on TCP and on slow devices. On a 115 200-baud link 1024 changing values pushed still need about 1.2 s, so only "changed since" fits 1 s, and only while most of them hold still; "Stale" must then follow a device heartbeat, not the last change. | The biggest change. A second transmit buffer on the device (about 2 KiB on the NUCLEO, whose one buffer replies now wait on); a lease the host renews, since a serial link gives no sign that the host has gone; coalescing or dropping when a TCP peer stops reading; the subscription's life with the session. |
| **A compact ReadMany reply** (status and value; the client already knows the IDs, order and widths) | 7 of 12 bytes an item: 1024 4-byte values from 1.6 s to 1.0 s of airtime at 115 200 baud. | A new reply form behind a capability; the reply no longer echoes IDs for the client to check. |
| **More items a request** (32 to 64) | Half the exchanges: 1024 values save about 0.16 s of turnarounds at 115 200 baud, and about 7 % of airtime. | A wire change: the limit of 32 is fixed at compile time and today's clients refuse more. The NUCLEO's 2048-byte buffers hold 64 items, not 128; the real cost is stack, since the per-call arrays grow with the limit. |
| **A member or slice read** (offset and length inside a Field) | Bytes when a few values of a large Field are watched: three 4-byte members of a 4 KiB struct take about 128 bytes instead of 4160, and can be read over a serial link that cannot carry the whole Field. | A new operation. The device still produces the whole value (getters return it whole), so it needs scratch for it or a walk by offset: wire bytes are saved, not processor time. |
| **ReadMany of any type** | 8 bytes an item against a Batch; on a device without Batch, an exchange for every struct after the first (worked out below). | A wire change behind a capability, a reply budget large enough for structs, and every client's portion planner by bytes. Not now: the owner, 14 November 2026, "if it is complex, do not complicate ReadMany for now". |

## Still to measure

All on test images and the demo device; nothing here touches production
firmware.

- **Where the 10 ms go.** A logic analyser on the NUCLEO's UART pins: the time
  from a request's final delimiter to the reply's first start bit is the
  device's own turnaround; the rest is the USB serial bridge and the host.
  Sweep FieldRead and ReadMany at 1, 8, 16 and 32 items at 115 200 and 921 600
  baud and fit latency = a + b x bytes for each rate; repeat through a
  separate USB-UART adapter (its latency timer set - FTDI's default is 16 ms).
- **The device's processor cost per item** for ReadMany and Batch of scalars,
  structs and arrays, with the H7S cycle-counter harness on a separate image.
- **TCP.** The demo device's turnaround, and a packet capture around replies
  of about 500, 540 and 1100 bytes for delayed-acknowledgement stalls.
- **The Client at 1024 Watchlist rows**: processor, frame time, recording rate
  and database growth (Client roadmap WL-6).

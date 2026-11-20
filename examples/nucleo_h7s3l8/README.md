# NUCLEO-H7S3L8 Inspector device over ST-LINK VCP

This is a standalone, maintained demonstration for **STM32H7S3L8H6** on
NUCLEO-H7S3L8. It has no Ethernet, Qt, RTOS, fiber scheduler or RPC mailbox.
The `while (1)` owner composes the existing external COBS UART DMA driver,
COBS fixed pools, Telemetry Model, Resource provider and Encoded dispatcher.
The example does not modify the instrument firmware or either existing H7S
bench project. Published voltages are demonstration constants, not ADC readings.

USART3 uses **PD8 TX / PD9 RX**, GPDMA1 channel **10 TX / 11 RX**, 115200 baud,
8 data bits, no parity, one stop bit and no flow control. These pins connect to
the board's ST-LINK VCP under its standard solder-bridge routing. The driver uses
normal ReceiveToIdle DMA with chunk switching, not a second DMA implementation.
UART and RX DMA share preemption priority 5. HSE is 24 MHz; the configured core
clock is 600 MHz, AXI 300 MHz and APB1 150 MHz, matching the local H7S bench.

## Wire profile and ownership

The shared [Profile.hpp](../../lib/integration/serial/Profile.hpp) selects
`cobs::Format<crc::Crc16Bitwise, 2048>`: a two-byte little-endian body length,
ordinary payload, existing CRC16 and COBS delimiter. Each decoded body is one
unchanged Resource request/reply or TEQ1 request/reply. There is no route byte,
TCP stream framing, RPC header, new wire operation or automatic resend.

The supported profile is one exclusive, sequential host: one outstanding
exchange and no later bytes from a retired producer after fresh bootstrap starts.
The default profile accepts only Resource LIST/STAT/READ and TEQ1 FieldRead. Resource
WRITE and TEQ1 FieldWrite/Command/Service are dropped before provider/Model dispatch.
The Model itself has no Commands or Services and all Fields are read-only.

[ServerLoop.hpp](ServerLoop.hpp) pops at most one complete COBS packet per step.
It does not dispatch while any of these remain: the UART's physical TX borrow,
the COBS endpoint's unreclaimed TX lease, or its single pending reply. A fresh
LIST therefore cannot overtake old published RX work or old UART transmission.
Providers execute synchronously; there is no deferred work or execution queue.
The default profile has no pinned TCP-style session: a valid read uses the current
request's nonzero TEQ1 session, and the host correlates its response.

A fresh **validated bootstrap transcript**, including LIST, can establish an
old-work drain boundary only under that restricted host/server profile. Closing
a COM port or clearing an OS buffer does not prove remote closure; Resource
replies have no transaction identity. The host's recovery/admission policy must
retain that distinction. This example does not promise reliable mutation or
exactly-once execution across reconnect.

The entire Uart object and COBS pool live in AXI SRAM `.bss` at `0x24000000`,
accessible to GPDMA. I/D caches are enabled. RX chunks fill aligned 32-byte cache
lines; the existing driver invalidates RX and cleans TX. DMA storage never lives
in DTCM. The COBS endpoint owns transmitted encoded bytes until the driver reports
physical release, normally UART TC after the last stop bit, including error
recovery. An exhausted DMA counter alone does not release the lease. Main owns
parsing/dispatch; interrupt callbacks only drive the existing UART state machine.

## Descriptor and demonstration fields

LIST discovers `/telemetry/descriptor.bin` after a version-file decoy; Descriptor
is deliberately file index 1. Clients must discover its index and read its full
size using STAT and cursor READ. TEQ1 bytes are unchanged.

| Field | Endpoint ID | Type | Value |
| --- | ---: | --- | --- |
| `measure.Voltage` | 0 | `struct nucleo_demo::Voltage` | Ua 230.1, Ub 229.8, Uc 231.2 |
| `device.Counter` | 65536 | `u64` | `18446744073709551615` |
| `device.Status` | 65537 | `struct nucleo_demo::Status` | U32 uptimeMs, requests, fieldReads, refused; Bool demonstration=true |
| `device.Probe` | 65538 | `struct nucleo_demo::Probe` | Matrix `[[1,-2,3],[4,5,-6]]`, Enabled=true, `enum nucleo_demo::Mode` mode=-2 |

### Type names

The descriptor is v3.1: it names its enum and structs, as above. The names
come from the code with nothing written for them: the innermost namespace and
the type's own name. `Mode` keeps its `EnumReflection`, which lists its
enumerators (Idle=-7, Active=12); its type name is automatic like the others.
A `TypeReflection<T>` specialisation in [Protocol.hpp](Protocol.hpp) could
replace a name (`typeName("board::Mode")`) or remove it (`noTypeName()`).

The commented [telemetry_config.hpp](telemetry_config.hpp) shows the
project-wide switch. `#define TELEMETRY_TYPE_NAMES 0` leaves every name out,
and the descriptor is then byte for byte the v3.0 one: hosts older than the
SDK that introduced v3.1 refuse a named descriptor, so a project that must
serve them either rebuilds its hosts or turns the names off. The model itself
stays the flat `Model{fields, commands, services}`, whose endpoint IDs above
do not change. The image size with and without names is in the
[ARM descriptor size report](../../tests/README.md#arm-descriptor-size-report).

The enum declares Idle=-7 and Active=12; -2 must display as an unknown signed code.
Status is captured by the single owner at dispatch. Its counter values include
that Status request and are diagnostics, not metrology quality information.
The exported volatile `telemetry_h7s_diagnostics` symbol gives SWD-readable magic,
loop/packet/reply/refusal counts, TX failures and UART overrun/error/restart counts.
There is no text console mixed into the protocol stream.

## Offline build

Dependencies remain external and retain their [original licenses](LICENSES.md):

- a COBS checkout at `2e0abf260848fcb74e4a57b37532188047759643`, with its
  `libs/spsc` submodule initialized;
- STM32CubeH7RS, qualified here with `STM32Cube_FW_H7RS_V1.3.0`;
- STM32CubeIDE's ARM GNU compiler; CMake and Ninja.

Prepare the external COBS dependency once; only its SPSC submodule is needed:

```powershell
git clone https://github.com/shpegun60/cobs C:\path\to\COBS
git -C C:\path\to\COBS checkout 2e0abf260848fcb74e4a57b37532188047759643
git -C C:\path\to\COBS submodule update --init libs/spsc
```

This example uses Telemetry's included `tiny_delegate.hpp`. The COBS delegate,
STM32F1 and STM32G4 submodules are not dependencies of this firmware.

From the Telemetry root, use a fresh output directory outside all source trees:

```powershell
examples\nucleo_h7s3l8\Build.ps1 `
  -CobsRoot C:\path\to\COBS `
  -OutputDirectory C:\artifacts\telemetry-h7s-firmware
```

`-Stm32CubeRoot`, `-ArmGccBin`, `-CMake`, `-Ninja` and `-Jobs` are configurable.
The script finds CMake and Ninja on `PATH` first, then checks the conventional
Qt Tools locations. Explicit tool overrides accept executable paths or command
names, such as `-CMake cmake -Ninja ninja`; a Qt installation is not required.
The script resolves the installed CubeIDE compiler and rejects CubeCLT. It builds
C++20 with `-Os` and LTO against SDK sources in place. The internal 64 KiB Flash
linker region is enforced; no external-flash loader or RAM-run fallback is used.
AXI SRAM holds data; DTCM holds the main stack. The SDK startup initializes static
storage and constructors before `main`; the application never exits.

The external receipt contains ELF/BIN/MAP, LTO `.su` stack reports, exact build
commands, compiler identity, tool hashes and before/after source hashes. A linked
allocator symbol fails qualification. `.su` rows describe individual compiler
frames, not measured maximum stack including nested interrupts.
This script never opens a serial port, invokes ST-LINK or programs a board.

## Optional scalar ReadMany profile

The default build preserves the four-Field Inspector demonstration above.
Pass `Build.ps1 -ReadMany` (or CMake `-DNUCLEO_ENABLE_READ_MANY=ON`) to compile
`TELEMETRY_NUCLEO_READ_MANY=1` and the two optional ReadMany server sources.
The opt-in firmware also accepts read-only TEQ1 opcode 5; mutation operations
remain refused. It uses the same UART, COBS endpoint, fixed pools and loop.

This profile appends `/telemetry/read-many.bin` as ordinary Resource file index
2; Descriptor remains index 1. Discover both paths with LIST and read their full
STAT-reported sizes using cursor READ. The capability has canonical 24-byte TRM1
bytes with the current Descriptor fingerprint, 32-item limit, 130-byte request
payload limit and 514-byte reply payload limit. No new Resource tag is added.

Before discovery the capability file is empty and both FieldRead and ReadMany
are refused. A validated successful LIST at cursor 0 that returns at least one
whole path advances the Protocol-owned discovery epoch and publishes it as the
capability session. Malformed LIST, insufficient reply space and nonzero LIST
cursors never advance it. STAT and READ freeze the advertised bytes. Both read
operations must carry this session; another successful initial LIST retires the
old one. Epoch UINT32_MAX is usable once; the next initial LIST empties the
capability and permanently closes read admission for that Protocol instance.
It never wraps or reuses a session within that instance.

Protocol owns its capability bytes, BytesFile and file table at stable addresses
and cannot be copied or moved. The optional `Protocol(retiredEpoch)` constructor
can seed a restored last epoch; the demonstration starts at zero. This is an
in-process discovery boundary, with no persistent boot identity promise. The
same exclusive sequential host and validated bootstrap drain conditions above
still apply. The UART borrow, unreclaimed COBS lease and pending-reply gate keep
old published work ahead of the next initial LIST.

The original four Field IDs remain unchanged. Catalogs 2 through 5 append:

| Catalog | Fields | Native values for index i |
| --- | --- | --- |
| `batch32` | `U32_0` ... `U32_7` | U32 `0x10203040 + i * 0x01010101` |
| `batch64` | `U64_0` ... `U64_7` | U64 `UINT64_MAX - i * 0x0102030405060708` |
| `flags` | `Bool_0` ... `Bool_7` | Bool `i % 2 == 0` |
| `modes` | `Mode_0` ... `Mode_6` | Signed int16 enum code `-2-i` |
| `modes` | `Unavailable` | Signed int16 enum with an empty owner slot |

Counter plus the 31 new readable getters provide exactly **32 readable scalar
Field IDs**. Unavailable is a separate scalar-typed ID; the three original
structured Fields remain available through FieldRead. There are 36 Field IDs
total. Nested struct/array members do not acquire separate Field IDs. Batch
sampling is sequential and does not promise an atomic snapshot. `fieldReads`
counts accepted read dispatches, so one batch increments it once; the fixture's
`calls` counter separately counts actual new scalar getter invocations.

Complete maximum TEQ1 bodies are 162 request bytes and 546 reply bytes, already
within the shared 2048-byte COBS body limit. Their worst-case physical sizes are
168 and 554 bytes including length, CRC and COBS framing. At 115200 baud, 8N1,
their combined airtime ceiling is 63 ms before scheduling allowance. Use the
existing `exchangeTimeoutMs(162, 546, baud, marginMs)` for a finite exchange
budget. DMA and endpoint leases remain owned through physical UART release.

## Host contract checks

```powershell
python -B examples/nucleo_h7s3l8/tests/run.py `
  --cobs-root C:\path\to\COBS `
  --build-dir C:\artifacts\telemetry-h7s-host `
  --cxx C:\Qt\Tools\mingw1310_64\bin\g++.exe `
  --cmake C:\Qt\Tools\CMake_64\bin\cmake.exe `
  --ninja C:\Qt\Tools\Ninja\ninja.exe
```

Linux may use `--cxx clang++-18 --sanitize`. These checks compile the **actual**
Protocol and ServerLoop. Real COBS fixed pools encode/decode both directions;
allocation traps reject hidden C++ heap use. They hold UART busy, then separately
hold the COBS lease, proving a queued LIST cannot bypass either. An old read is
completed before fresh LIST. Other cases verify Descriptor discovery/bytes,
refusal of every mutation operation, corrupt CRC, truncated-frame gap recovery,
new sessions and no automatic retry after TX-start refusal.

The same runner also builds and executes the opt-in profile. Its maintained
checks cover 8/12/16/32 cross-catalog batches, exact scalar bytes against owned
FieldRead baselines, per-item failures, preflight before getters, paginated
Descriptor reads, capability/epoch stability and exhaustion, and the same real
COBS CRC, gap recovery and independent physical-TX/lease barriers. ReadError is
injected only into a host-side copied erased row; the firmware's native typed
getters do not expose a partial-read failure API. These offline checks do not
replace hardware measurements or alter the retained default-profile receipts.

Hardware qualification is separate: verify board identity and backup first,
program only under explicit authorization, use the actual VCP device name,
then check Descriptor/fields, reconnect, corrupted frames, DMA/cache diagnostics
and lifetime. Neither an offline link nor a host test proves the physical UART,
ST-LINK USB path, cache coherency or stack/CPU budgets on a connected board.

# Reading a Field without the SDK

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

[User guide](README.md) · [API map](APIOverview.md) · [Wire format](../WireV3.md) ·
[Request and reply frames](../WireV3.md#request-and-reply-frames-teq1-v1)

A client in any language can talk to a device with a TCP socket and this page.
It reads one Field, `adc/channel1/Gain` of the
[group_tree example](../../examples/group_tree/README.md), an `f32`, and shows
every byte sent and received. The Field's ID is the one the client's Details
shows for it: `1:0`, packed `0x00010000`.

Every hex block on this page comes from the real TCP server, encoded profile
and codec, running the example's Model. [tests/field_by_hand](../../tests/field_by_hand/README.md)
compares each block with the server's bytes, so this text cannot drift from the
code. The client's emulator answers the same way; only its IDs, sizes and
fingerprint are its own, and its Details shows them.

## What you need

- A TCP connection to the device.
- The Field's ID and size. Step 3 takes them from the descriptor. The client's
  Details shows the same values: `1:0`, `f32`, 4 bytes.
- The descriptor's fingerprint, if you keep an ID between runs (step 2).

Rules for every step:
- All integers are little-endian.
- Send one request, read its whole reply, then send the next.
- In the blocks below, the text after `;` explains the bytes before it.

## 1. List the files

A Resource LIST from cursor 0:

<!-- bytes:list-request -->
```text
01                         ; LIST
00 00 00 00 00 00 00 00    ; cursor 0 (u64)
```

The reply lists every path, each with a u16 length in front. The first path's
index is the request's cursor, 0, and each next one adds 1:

<!-- bytes:list-reply -->
```text
00                         ; status Ok
04 00 00 00 00 00 00 00    ; next cursor 4 (u64)
01                         ; eof: that was every file
63 00                      ; 99 bytes of entries follow (u16)
19 00                      ; index 0, 25 bytes:
2f 74 65 6c 65 6d 65 74 72 79 2f 64 65 73 63 72 69 70 74 6f 72 2e 62 69 6e   ; /telemetry/descriptor.bin
16 00                      ; index 1, 22 bytes:
2f 74 65 6c 65 6d 65 74 72 79 2f 73 65 73 73 69 6f 6e 2e 62 69 6e   ; /telemetry/session.bin
18 00                      ; index 2, 24 bytes:
2f 74 65 6c 65 6d 65 74 72 79 2f 72 65 61 64 2d 6d 61 6e 79 2e 62 69 6e   ; /telemetry/read-many.bin
14 00                      ; index 3, 20 bytes:
2f 74 65 6c 65 6d 65 74 72 79 2f 62 61 74 63 68 2e 62 69 6e   ; /telemetry/batch.bin
```

This device lists `/telemetry/session.bin`, so step 4 binds a session before
the first Field request. The [Resource protocol](../../lib/resource/protocol/README.md)
defines LIST, STAT, READ, WRITE and SAVE and their statuses.

## 2. Read the descriptor and check its fingerprint

This step is optional when you already know the ID and the size. A READ of file
0, `/telemetry/descriptor.bin`, from cursor 0:

<!-- bytes:read-request -->
```text
03                         ; READ
00 00 00 00                ; file index 0 (u32)
00 00 00 00 00 00 00 00    ; cursor 0 (u64)
```

The reply starts with a 12-byte envelope, and the file's bytes follow it:

<!-- bytes:read-reply-envelope -->
```text
00                         ; status Ok
38 05 00 00 00 00 00 00    ; next cursor 1336 (u64)
01                         ; eof
38 05                      ; 1336 bytes of data follow (u16)
```

Here the whole 1336-byte descriptor fits one reply. A device with a smaller
buffer sends less: READ again from `next` until `eof`. The first 64 bytes of
data are the descriptor header ([format](../WireV3.md#descriptor-header)):

<!-- bytes:descriptor-header -->
```text
54 44 53 33                ; TDS3
03 00 01 00                ; major 3, minor 1
40 00 01 00                ; headerBytes 64; flags 1 = GroupTree
38 05 00 00                ; totalBytes 1336
08 48 ff b6 da 63 ca 33    ; fingerprint 0x33ca63dab6ff4808 (u64)
16 00 00 00                ; typeCount 22
04 00 00 00 06 00 00 00    ; 4 Field catalogs (one per group), 6 Fields
04 00 00 00 01 00 00 00    ; 4 Command catalogs, 1 Command
04 00 00 00 01 00 00 00    ; 4 Service catalogs, 1 Service
40 00 00 00                ; typesOffset 64
c0 03 00 00                ; catalogsOffset 960
4c 04 00 00                ; endpointsOffset 1100
```

The fingerprint, bytes 16 to 23, identifies this descriptor; the client shows
it beside the device. An ID is a position in one descriptor, so keep the
fingerprint with every ID you store. If the device still reports
`0x33ca63dab6ff4808`, `1:0` is still `adc/channel1/Gain`. If the fingerprint
changed, find the Field again (step 3).

## 3. Find the Field's ID and size

A client does not compute an ID. It reads the ID from the descriptor:
1. The group records come first, in pre-order: `adc` is group 0,
   `adc/channel1` is 1, `adc/channel2` is 2 and `metrology` is 3. A record's
   depth gives its parent.
2. The endpoint records start at `endpointsOffset`. Fields come first, group by
   group, in declaration order.
3. Group 0 has two Fields, `Config` and `Status`, so the third Field record is
   the first Field of group 1, `adc/channel1/Gain`:

<!-- bytes:gain-record -->
```text
03 01 00 00                ; record: kind 3 = Field, version 1, flags 0
14 00 00 00                ; payloadBytes 20
00 00 01 00                ; packedId 0x00010000 = 1:0 (group 1, entry 0)
0a 00 00 00                ; valueTypeId 10
03 00 00 00                ; capabilities 3 = Readable | Writable; 3 reserved zero bytes
04 00 00 00 47 61 69 6e    ; name: 4 bytes, "Gain"
```

- The path is the group's path and the record's name: `adc/channel1/Gain`.
- Type 10 is the scalar `f32`, and its type record gives `wireBytes` 4.
  This SDK's producer always emits Void and the eleven scalars first, as
  TypeIds 0 to 11, so `f32` is 10 in every descriptor it makes. A client
  still looks a type up by its TypeId.
- Capabilities 3 means the Field can also be written (operation 1, FieldWrite).
- [PackedId](../WireV3.md#packedid) and
  [IDs from the descriptor](../WireV3.md#ids-from-the-descriptor-and-the-fingerprint)
  give the rules.

## 4. Bind a session

The session is a nonzero number that every TEQ1 request of this connection
carries. Write it into file 1, `/telemetry/session.bin`, as four bytes:

<!-- bytes:session-request -->
```text
04                         ; WRITE
01 00 00 00                ; file index 1 (u32)
00 00 00 00 00 00 00 00    ; cursor 0 (u64)
01                         ; final
04 00                      ; 4 bytes of data follow (u16)
01 00 00 00                ; session 1 (u32)
```

<!-- bytes:session-reply -->
```text
00                         ; status Ok
04 00 00 00 00 00 00 00    ; next cursor 4 (u64)
04 00 00 00                ; 4 bytes consumed (u32)
01                         ; complete
```

A session is written once per connection. A device that does not list
`session.bin` has no such step: its first TEQ1 request binds that request's
session to the connection.

## 5. Read the Field

The request is a 32-byte TEQ1 header with no payload:

<!-- bytes:field-read-request -->
```text
54 45 51 31                ; magic "TEQ1"
01                         ; version 1
00                         ; flags 0: a request
04                         ; operation 4: FieldRead
00                         ; dispatch: 0 in a request
01 00 00 00                ; session 1
01 00 00 00                ; requestId 1
00 00 01 00                ; endpoint 0x00010000 = 1:0, adc/channel1/Gain
00 00 00 00                ; payloadBytes 0: a read sends no value
04 00 00 00                ; expectedResponseBytes 4: the f32's wireBytes
00                         ; endpointStatus: 0 in a request
00 00 00                   ; reserved
```

The reply echoes the header with flags 1, and the value follows it:

<!-- bytes:field-read-reply -->
```text
54 45 51 31                ; magic "TEQ1"
01                         ; version 1
01                         ; flags 1: a reply
04                         ; operation 4, echoed
00                         ; dispatch 0: Ok
01 00 00 00                ; session, echoed
01 00 00 00                ; requestId, echoed
00 00 01 00                ; endpoint, echoed
04 00 00 00                ; payloadBytes 4
04 00 00 00                ; expectedResponseBytes, echoed
00                         ; endpointStatus 0
00 00 00                   ; reserved
00 00 80 3f                ; the value
```

The four payload bytes, read little-endian, are the IEEE 754 binary32 pattern
`0x3f800000`: 1.0, the gain the example starts with. A wider type sends its
[canonical encoding](../WireV3.md#value-encoding): a struct sends its members in
order, without padding.

## Next requests and errors

- Give each request a larger `requestId`: 2, 3 and so on. Keep the session.
- The device closes the connection, without answering, on a malformed frame,
  on a session other than the bound one and on a `requestId` that is not
  larger than the previous one.
- A reply whose `dispatch` is not 0 carries no value:

| `dispatch` | Meaning | What to check |
| --- | --- | --- |
| 4 NotFound | No Field has this ID | The ID is stale, or it names a Command or Service: the operation selects the family |
| 5 InvalidPayload | The sizes do not match the type | `payloadBytes` and `expectedResponseBytes` must equal the descriptor's `wireBytes` |
| 6 BufferTooSmall | The reply does not fit the device's buffer | A smaller value, or a device with a larger buffer |
| 7 WorkspaceTooSmall, 8 InternalError | The device could not run the operation | The device's configuration |
| 9 Unavailable | The Field's owner is not bound: its late-bound slot is empty | The device binds it; read again later |

`dispatch` 0 means the device ran the operation. For a write, a Command and a
Service, `endpointStatus` then says whether the application accepted it.

## Writing a Field, calling a Command or Service

The header stays the same; the operation and the sizes change:

| Operation | `payloadBytes` and payload | `expectedResponseBytes` | Reply |
| --- | --- | --- | --- |
| 1 FieldWrite | the value, `wireBytes` of the Field | 0 | `endpointStatus` is a `WriteResult`; 0 Applied |
| 2 Command | the request struct, or 0 without one | 0 | `endpointStatus` is a `CommandResult`; 0 Executed |
| 3 Service | the request struct, or 0 without one | `wireBytes` of the response | `endpointStatus` is a `ServiceStatus`; the response follows when it is 0 |

Every code is in [Request and reply frames](../WireV3.md#request-and-reply-frames-teq1-v1).
A Command or Service ID comes from its own record (kind 4 or 5) in the same
way as in step 3.

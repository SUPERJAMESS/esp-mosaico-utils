# ESP-Iris protocol 0.2 (wire version 2)

This is the active contract for the 0.2 implementation. It is incompatible with
0.1. The historical contract and vectors live in `archive/0.1/`; they are not
accepted by the current device or host. Service-specific schema/profile numbers
are independent of the outer wire version.

## Links and console ownership

A device has one control session and, when configured, one independent data
session. USB HS exposes CDC0 (`ESP-Iris 0.2 console`) and CDC1
(`ESP-Iris 0.2 data`) under one stable device serial. TCP listens on separate
ports, default 19772 for control and 19773 for data. UART and USB Serial/JTAG
are control-only. Transport values are USB=1, TCP=2, Serial/JTAG=3, UART=4;
NONE=0 is a local status value, never a HELLO transport.

Control is ordinary text: standard logs continue before, during and after Iris
sessions. Stock `idf.py monitor` needs no Iris host software. Monitor and Gateway
exclusively own the control endpoint; no device mode switch is involved.
Iris never disables Serial/JTAG reset/download behavior. Reconnection does not
request a reset. HS CDC cannot provide bootloader logs before its stack starts;
use a native UART/Serial-JTAG path for early boot and fault diagnostics.

Products with an existing UART/Serial-JTAG REPL select
`CONFIG_ESP_IRIS_CONSOLE_EXTERNAL_INPUT`, initialize their REPL/driver, set its
maximum line length to `ESP_IRIS_CONSOLE_LINE_BYTES`, and register the `iris`
namespace with `esp_iris_console_register_commands()`. Custom dispatchers can
submit complete lines with `esp_iris_console_submit()`. The product remains the
sole input reader and driver owner. Its driver RX queue must hold at least two
complete records; the default 256-byte interactive Serial/JTAG ring is too small.
Configure it before starting the product REPL, not while Iris borrows it.
Iris queues one bounded command and responds
asynchronously; queue-full returns ESP_ERR_TIMEOUT. Stop Iris before the REPL.
Without this option the Iris worker owns the configured console input.

### Printable records

```text
host:   iris @BASE64(COBS(v2 header || payload || crc32) || 00) CR LF
device: @iris/0.2 BASE64(COBS(v2 header || payload || crc32) || 00) CR LF
```

Base64 is canonical RFC 4648 with padding and no whitespace inside it. Each
record, including its prefix and line terminators, is bounded by 5476 bytes.
Firmware puts a CR/LF before a response record to separate partial log lines;
native console logging is serialized around the complete record. Invalid,
truncated or oversized input is discarded through a line boundary. Ordinary
text and invalid machine-looking output remain raw capture evidence; only a
fully validated frame is dispatched as a response.

`iris`, `iris help`: list commands; `iris status`: readable identity/health;
`iris hello`: one discovery response; `iris @...`: all bounded control services,
including RPC, jobs, input and snapshot OPEN/READ/CLOSE. CR or LF terminates a
command; backspace/delete edits the current input. Normal console output has
no unsolicited binary HELLO or heartbeat. A Gateway may retry the explicit
HELLO query while continuing to capture raw boot logs.

Data uses the binary envelope below. It may connect first and advertises HELLO
while negotiating. USB descriptors, mDNS and control HELLO endpoint hints select
candidates only. Pairing by adjacent tty numbers or IP address alone is invalid.
The data handshake independently verifies version, device, boot and authority.

The two links must use the same nonzero 16-byte owner token and actual device/
boot identity. Session IDs are independently random, nonzero and distinct.
A second link claiming another owner is rejected. A task binds its service
object/stream/operation ID to its data session and therefore to the authenticated
owner and boot. Disconnecting control preserves a healthy data session; loss
of data aborts its live streams/transfers while preserving control. Reboot or
final owner release invalidates all bindings. No mutation is automatically
replayed after reconnect.

## Envelope

Each frame is encoded as:

```text
COBS(header || payload || crc32) || 0x00
```

All integers are little endian. CRC32 is IEEE CRC-32 as implemented by zlib,
over the decoded header and payload. The delimiter is not covered. Receivers
drop invalid bytes until the next `0x00` delimiter.

The decoded 32-byte header is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII `IRIS` |
| 4 | 1 | protocol version (`2`) |
| 5 | 1 | header size (`32`) |
| 6 | 1 | channel |
| 7 | 1 | type, scoped by channel |
| 8 | 2 | flags |
| 10 | 2 | reserved, must be zero |
| 12 | 4 | session ID |
| 16 | 4 | request ID, or zero |
| 20 | 4 | stream ID, or zero |
| 24 | 4 | per-channel sequence |
| 28 | 4 | payload size |

Maximum payload is 4000 bytes and maximum encoded frame, including delimiter,
is 4096 bytes. A large object uses service-specific OPEN/DATA/CLOSE frames;
it is never placed in one oversized envelope.

Channels are `CONTROL=0`, `LOG=1`, `EVENT=2`, `SCREEN=3`, `IMAGE=4`,
`AUDIO=5`, `OTA=6`, `CRASH=7`, `FILE=8`, and `SYSTEM_UPDATE=9`. FILE is sent
only when both peers recognize `CAP_FILE` (capability bit 13).
`CAP_OTA_PROJECT_NAME_MATCH`
(capability bit 14) advertises that the running firmware requires an OTA
image's project name to match its own. An absent bit means that cross-project
updates are allowed. `CAP_CRASH_LOOP` (capability bit 18) advertises retained
reset attribution and consecutive-crash fields. Unknown types on a known channel produce a CONTROL ERROR
when a response is possible. A future protocol version must use capability
negotiation rather than silently reinterpreting an existing type.

## Control session

The Gateway allows CONTROL `STATUS_REQUEST`, `PING`, and `TIME_SYNC_REQUEST` to
wait concurrently with an in-flight RPC. Each has a distinct request ID and all
frames still share serialized per-link writes and sequence allocation. Other
requests retain the mutation lock; this does not bypass operation/device locks.
Closing a session fails every pending request and prevents queued mutations from
being sent. Thus live status remains observable while the device RPC executor is
busy, without treating another queued RPC as a liveness probe (IRIS-A02).

Each link chooses a nonzero random session ID. Data repeats HELLO while
negotiating; control emits HELLO only in response to `iris hello`. The PC echoes
the session ID in every frame. Old-session frames are discarded. HELLO_ACK is
`role:u8 || owner_id[16]` on local links and appends the TCP proof below when
pairing is enabled. Role is 0 for control and 1 for data and must match the
endpoint. Every accepted ACK receives AUTH_RESULT(1), including local links;
readiness requires that result, not merely a HELLO.

After the first HELLO_ACK in a session, the device emits exactly one BOOT event
for that session. A repeated HELLO or HELLO_ACK must not create another BOOT
event. BOOT is deliberately replayed after a link reconnect so a new PC Hub
can recover boot metadata; it does not by itself mean the device rebooted.
Consumers compare `boot_id` to detect a real boot and `session_id` to detect a
new physical link session.

After HELLO_ACK, replay order is BOOT, LINK_READY, optional
PREVIOUS_BOOT_CRASH, optional CORE_DUMP_AVAILABLE, optional
CRASH_LOOP_DETECTED and optional HEALTHY.
`esp_iris_mark_healthy()` updates replayable lifecycle state. A planned restart
event records local intent in Iris NVS. Products may still override the
platform hook for additional product metadata. Event type
`0x02` is reserved and must not be reinterpreted.

Implemented control types:

| Type | Value | Payload |
|---|---:|---|
| HELLO | `0x01` | TLV device description |
| HELLO_ACK | `0x02` | role + owner binding, optionally TCP nonce + proof |
| PING/PONG | `0x03/0x04` | opaque echoed bytes |
| TIME_SYNC_REQUEST | `0x05` | host monotonic `t1_ns: u64` |
| TIME_SYNC_RESPONSE | `0x06` | `t1_ns, device_d2_us, device_d3_us: u64` |
| STATUS_REQUEST | `0x07` | empty |
| STATUS_RESPONSE | `0x08` | TLV status |
| CREDIT | `0x09` | `channel:u8, reserved[3], bytes:u32` |
| REQUEST/RESPONSE | `0x10/0x11` | bounded binary RPC |
| CANCEL | `0x12` | `job_id:u32` |
| JOB_QUERY/JOB_STATUS | `0x13/0x14` | job ID / fixed job status |
| RESTART | `0x15` | `delay_ms:u32` |
| AUTH_RESULT | `0x16` | `accepted:u8` |
| TASKS_REQUEST/TASKS_RESPONSE | `0x17/0x18` | empty / task stack snapshot |
| ERROR | `0x7f` | `esp_err:u32, channel:u8, type:u8, reserved:u16` |

CONTROL and reliable EVENT traffic are not charged against media/log credit.
Ordinary control logs require no handshake or credit. Bulk-only services are
rejected on control with NOT_SUPPORTED: FILE, continuous media and firmware
content. SCREEN OPEN/READ/CLOSE and read-only diagnostics remain available.

## TLV

Control and event metadata use:

```text
tag:u8 || length:u16 || value[length]
```

The value's scalar encoding is defined by the tag. Unknown tags are skipped.
Strings are UTF-8 without a terminating NUL. Device ID is 16 raw identity bytes;
boot ID and capabilities are `u64`.

The stable device ID is the 10-byte domain prefix `ESP-IRIS 01 00` followed by
the six-byte factory eFuse Base MAC. `HARDWARE_MAC` carries those same final six
bytes explicitly. This keeps the existing 16-byte identity shape while making
ROM download, Recovery and normal firmware derive exactly the same identity
without NVS. The boot ID remains random on every boot.

STATUS includes lifecycle state, link/invalid-frame counters, minimum worker
stack headroom, maximum active worker-loop time, startup internal-heap delta
and static Iris bytes. The PC reports `internal_total_bytes` as static bytes
plus Iris heap usage. Optional service allocations created after start are
included. Mirror buffers are released on stop; registered RPC/screen metadata
and retained jobs stay bounded by Kconfig.

STATUS also includes allocator `TOTAL_INTERNAL` (`0x2c`), `TOTAL_SPIRAM`
(`0x2d`), `FREE_SPIRAM` (`0x2e`), and `MIN_FREE_SPIRAM` (`0x2f`) as `u32`
byte counts. `FREE_INTERNAL` (`0x20`) and `MIN_FREE_INTERNAL` (`0x21`) retain
their existing meaning. A zero SPIRAM total means no allocatable SPIRAM was
registered. The minimum-free values sum per-region low watermarks, which may
have occurred at different times; they are not an exact simultaneous global
minimum. Unavailable allocators report zero totals.

When `CAP_TASK_MEMORY` (bit 19) is advertised, TASKS_REQUEST returns a single
bounded TASKS_RESPONSE:

```text
sample_uptime_us:u64
count:u16
reserved:u16 = 0
repeat count times:
    task_number:u32
    stack_free_min_bytes:u32
```

`task_number` is the FreeRTOS task number for this boot, not a pointer or a
cross-boot identity. `stack_free_min_bytes` is the task's lifetime minimum
remaining stack in ESP-IDF FreeRTOS byte units. The snapshot includes live
tasks returned by `uxTaskGetSystemState()` and excludes tasks already marked
deleted; tasks may be created or deleted between polls. A request with a body
is rejected. If more than 128 tasks are
present, the device returns CONTROL ERROR with `ESP_ERR_INVALID_SIZE` rather
than silently truncating the list. The query allocates temporary storage and
scans stacks only on request; it has no device-side polling task.

HELLO additionally requires LINK_ROLE (`0x17`, u8), and advertises optional
DATA_TCP_PORT (`0x18`, u16) and DATA_AVAILABLE (`0x19`, u8). DATA_AVAILABLE means
firmware support, not that a peer is currently connected. The host uses live
control/data sessions to determine actual feature availability.

## Logs

Control transmits native text, not LOG-channel envelopes. Gateway starts reading
and storing `console_raw` records before sending HELLO: capture ID, endpoint,
host receive time, offset and Base64 original bytes. Unknown device/boot identity
is explicitly null. A later `console_binding` event associates the capture with
a verified device and boot at its recorded offset; it never retroactively
assigns early bytes a boot ID. Disconnect, reset and retention gaps are explicit.
Raw logs use the existing rotation/index/Follow APIs (`/v2`); use capture/endpoint
filters for unidentified or console-only firmware.

The S31 USB transport registers a shutdown handler while it owns the controller.
Before a software CPU reset it disables the USB interrupt, resets the HS USB
controller/PHY and gates its clocks. CPU reset alone in the pinned SDK leaves
DMA destinations from the previous image active; those addresses may contain
instructions in the next image. This shutdown path does not wait for the Iris
worker, which can itself initiate the restart. A product bootloader must also
stop inherited DMA before loading application RAM to cover panic/watchdog resets
that bypass application shutdown handlers.

When binding previously unidentified records, `console_binding` includes
`history_start_event_id` and `history_end_event_id`. Device-filtered Follow
subscribers receive those retained records before the binding event, including
when resuming across a binding that occurred while disconnected. Replayed records
retain their original event IDs and receive times; their unknown boot IDs stay
null. IDs can therefore arrive below the subscriber's current cursor: deduplicate
by event ID and advance the resume cursor with `max`, rather than discarding all
lower IDs. Unfiltered subscribers already received these records and do not get
this additional replay. Rotation still limits available history.

The LOG-channel record format below remains reserved for structured diagnostics;
it is not the wire representation of native console text.


LOG RECORD (`type=0x01`) payload:

```text
monotonic_us:u64
dropped_total:u32
source:u8             # 1 stdout, 2 stderr
flags:u8
length:u16
data[length]
```

The VFS writer is nonblocking. A full device ring drops the oldest records and
increments `dropped_total`; writes still report the original byte count to the
caller. The protocol implementation itself never calls printf or ESP_LOG.

## Time

Device event ordering always uses `esp_timer_get_time()` in microseconds. The
PC estimates offset with four timestamps and stores device monotonic time,
host receive time, estimated wall time, and uncertainty. Device wall clock is
not an ordering authority.

Every PC event carries an `event_id` derived from device ID, boot ID, device
monotonic time and per-channel sequence. A Hub classifies a new session as
`connected`, `reconnected` or `rebooted` by comparing the last boot ID seen for
that device. Sequence duplicates and backwards frames on a live session are
dropped.

## Crash evidence

The CRASH channel is read-only. It never erases a coredump or writes a crash
partition.

| Type | Value | Payload |
|---|---:|---|
| METADATA_REQUEST | `0x01` | empty |
| METADATA_RESPONSE | `0x02` | TLV crash report |
| READ_REQUEST | `0x03` | `offset:u32, maximum:u16, reserved:u16` |
| READ_RESPONSE | `0x04` | `offset:u32, total_size:u32, data[]` |

Metadata always reports reset reason and whether it represents a previous-boot
crash. When Flash coredump support is compiled and a valid coredump partition
is present, it also reports retained size, panic reason, coredump ELF SHA and
the maximum chunk size. READ responses set STREAM_END on the final chunk and
reuse the device RX frame buffer, so no media-sized or full-coredump allocation
is required.

The PC permits evidence download even when the embedded ELF SHA is incomplete.
It may select an archived ELF from either a complete 64-character coredump SHA,
or from the complete retained failed-firmware SHA when the coredump SHA is a
matching prefix from the same failed Boot ID. Decoding against an ELF without
one of these exact identity checks is outside the protocol contract.

When `CAP_CRASH_LOOP` is present, metadata and STATUS also expose the retained
`CRASH_COUNT`, configured `CRASH_LIMIT`, threshold/pending flags, original
failure reset reason, failed application address, failed Boot ID and failed
firmware SHA-256.
These fields survive the planned software restart into Recovery, while
`PREVIOUS_BOOT_CRASH` deliberately continues to describe only the immediate
reset reason. A Recovery boot can therefore report
`previous_boot_crash=false` together with `crash_recovery_pending=true` and the
original panic or watchdog reason.

The Core Dump contains a compact `g_iris_crash_context` record with the same
failed Boot ID and firmware SHA. A decoder uses it to reject a stale dump from
another boot of the same image.

Iris writes a single versioned `crash_loop` blob in namespace `esp_iris` at
boot. A panic, watchdog or CPU-lockup reset is attributed to the image address
and SHA recorded by the previous boot. Brownout and power-glitch resets are
excluded unless explicitly configured. A normal image clears the count after
the stable interval; accepting an installation with `esp_iris_mark_healthy()`
does not clear crash history. Recovery does not implicitly
clear another image's failure record. At the threshold a normal image selects
Recovery with `esp_iris_platform_select_recovery_target()` (factory by default),
commits the retained evidence, and performs a planned software restart.

## File service

The optional FILE channel exposes only application-registered logical volumes.
ESP-Iris never exports `/`, NVS, OTA partitions, coredumps, its log VFS, or any
mount automatically. A target is encoded as a logical volume ID plus a canonical
UTF-8 relative path:

```text
volume_length:u8, reserved:u8, path_length:u16
volume[volume_length], path[path_length]
```

Volume IDs contain 1-15 ASCII letters, digits, `_`, or `-`. The empty path means
the volume root. Nonempty paths cannot start or end with `/`, contain an empty,
`.` or `..` component, contain `\`, NUL, or control bytes, or exceed 255 encoded
bytes. ESP-IDF's supported LittleFS, SPIFFS, and FATFS VFS backends do not expose
symbolic links; unsupported file kinds are omitted or rejected.

Every FILE response begins with `status:u16, reserved:u16`. Status is a stable
protocol value, not a platform `errno`: `OK=0`, `INVALID_ARGUMENT=1`,
`NOT_FOUND=2`, `NOT_DIRECTORY=3`, `NOT_FILE=4`, `READ_ONLY=5`, `BUSY=6`,
`NO_MEMORY=7`, `IO=8`, `NOT_SUPPORTED=9`, `CONFLICT=10`, `EXISTS=11`,
`NOT_EMPTY=12`, `NO_SPACE=13`, and `HASH_MISMATCH=14`.

An application may register a volume before its backing filesystem is mounted.
Its descriptor and capabilities remain visible in `VOLUMES`. Path operations
return `BUSY` while the mount is pending, or `IO` after a mount failure. Once
mounted, requests succeed on the same session without reconnecting. Existing
status values and packet layouts are unchanged.

Implemented FILE types are:

| Type | Value | Request / response payload after status |
|---|---:|---|
| VOLUMES_REQUEST/RESPONSE | `0x01/0x02` | empty / `chunk_max:u16, path_max:u16, count:u8, reserved[3]`, then volume records |
| STAT_REQUEST/RESPONSE | `0x03/0x04` | path target / metadata |
| LIST_OPEN/OPENED | `0x05/0x06` | path target / `stream_id:u32` |
| LIST_NEXT/DATA | `0x07/0x08` | empty / page header and at most one entry |
| CLOSE/CLOSE_RESPONSE | `0x09/0x0a` | empty / empty |
| READ_OPEN/OPENED | `0x0b/0x0c` | path target / stream metadata |
| READ/DATA | `0x0d/0x0e` | read range / offset, total and bytes |
| WRITE_OPEN/OPENED | `0x0f/0x10` | path target plus write declaration / stream ID and chunk maximum |
| WRITE/ACK | `0x11/0x12` | strict offset and bytes / committed offset |
| COMMIT/COMMIT_RESPONSE | `0x13/0x14` | SHA-256 / final metadata |
| ABORT/ABORT_RESPONSE | `0x15/0x16` | empty / empty |
| MKDIR/MKDIR_RESPONSE | `0x17/0x18` | path target / metadata |
| DELETE/DELETE_RESPONSE | `0x19/0x1a` | path target / empty |
| RENAME/RENAME_RESPONSE | `0x1b/0x1c` | same-volume source and destination / metadata |
| WRITE_STATUS/WRITE_STATUS_RESPONSE | `0x1d/0x1e` | empty / resumable write state |

A volume record is `id_length:u8, reserved:u8, capabilities:u16, id[]`.
Capabilities are `READ=bit0`, `LIST=bit1`, `MTIME=bit2`, `WRITE=bit3`,
`DELETE=bit4`, `MKDIR=bit5`, `RENAME=bit6`, `ATOMIC_REPLACE=bit7`, and
`HASH=bit8`. `WRITE` always implies `HASH`; overwrite is rejected unless the
product also declares `ATOMIC_REPLACE`. Common metadata is:

```text
kind:u8              # 1 regular file, 2 directory
reserved:u8
reserved:u16
size:u64
mtime_s:u64          # zero when unavailable
opaque_etag:u64
```

LIST_OPEN returns the same nonzero stream ID in the envelope and payload and sets
STREAM_BEGIN. LIST_NEXT carries that ID in the envelope. LIST_DATA is
`end:u8, count:u8` after status; count is 0 or 1. An entry replaces metadata's
first reserved byte with `name_length:u8` and appends `name[name_length]`.
The terminal empty page sets `end bit0` and STREAM_END. Directory order and
cursor replay are not snapshot semantics. CLOSE releases the stream.

READ_OPEN similarly returns `stream_id:u32, total_size:u64, mtime_s:u64,
opaque_etag:u64, chunk_max:u16, reserved:u16` and sets STREAM_BEGIN. READ is
`offset:u64, maximum:u16, reserved:u16`; DATA is `flags:u16, offset:u64,
total_size:u64, data[]` after status. Requests are stop-and-wait and offsets are
64-bit. The last DATA sets STREAM_END and flags bit 0. Files are read by the
dedicated bounded, low-priority file task; only the Iris worker writes frames.
The task, its queues, and per-stream working state are created on the first FILE
request in a session and released when that session ends. Registered volume
metadata remains available so the advertised capability does not change.

WRITE_OPEN appends the following declaration to a nonempty path target:

```text
total_size:u64
if_match_etag:u64
flags:u16              # bit0 overwrite, bit1 if-match is present
reserved:u16
```

It creates an exclusive temporary file in the target's existing parent
directory. Creating an existing target returns `EXISTS`; overwriting requires
both the overwrite flag and the volume's `ATOMIC_REPLACE` capability. If-match
compares the opaque target ETag before opening and the device checks the target
again immediately before replacement. WRITE_OPENED returns
`stream_id:u32, chunk_max:u16, reserved:u16` and sets STREAM_BEGIN.

WRITE is `offset:u64, data_size:u16, reserved:u16, data[data_size]`. The offset
must equal the current committed offset, data must not exceed either the
declared total or chunk maximum, and ACK returns `committed_offset:u64`.
Requests are stop-and-wait. After an ACK timeout, the host queries WRITE_STATUS
instead of blindly retransmitting.

COMMIT contains exactly the SHA-256 of the declared file. The device requires
the exact declared byte count and hash, verifies that the destination did not
change, then performs `fsync`, close, and same-directory rename. Only after the
rename does it return final metadata and STREAM_END. It never degrades an
advertised atomic replacement into an in-place write. The temporary file needs
additional free space and is removed by ABORT, session loss, write failure, or
failed commit.

WRITE_STATUS uses the write stream ID and returns:

```text
committed_offset:u64
expected_size:u64
state:u8               # 1 active, 2 committed, 3 aborted
reserved[3]
result:u16              # stable FILE status for terminal state
reserved:u16
```

The device retains the last terminal receipt for the session so a host can
resolve a lost COMMIT response. A later WRITE_OPEN replaces that receipt.

MKDIR creates exactly one directory. DELETE removes a regular file or an empty
directory; recursive deletion is not defined. RENAME carries
`volume_length:u8, reserved:u8, source_length:u16, destination_length:u16,
reserved:u16, volume[], source[], destination[]`, stays within one logical
volume, and never overwrites an existing destination. The volume root cannot
be written, renamed, or deleted, and a directory cannot be moved below itself.
Only one LIST, READ, or WRITE handle is active
at a time; unrelated metadata operations remain bounded and mutations return
BUSY while a handle is active.

## RPC and jobs

CONTROL REQUEST payload:

```text
service_id:u16, method_id:u16, deadline_ms:u32
body_size:u16, reserved:u16, body[body_size]
```

CONTROL RESPONSE repeats service/method followed by
`result:i32, body_size:u16, reserved:u16, body[]`. Bodies are capped by
`CONFIG_ESP_IRIS_RPC_BODY_BYTES`; handlers receive binary spans and run on
the Iris worker, so they must not block. A response that finishes after its
relative deadline is returned as timeout. Reusing the last request ID in one
session is deterministically rejected.

Long operations use a bounded job record:

```text
job_id:u32, kind:u16, state:u8, cancel_requested:u8
progress_permille:u16, reserved:u16, result:i32
```

JOB_QUERY and CANCEL return JOB_STATUS. Updates are also emitted as reliable
EVENT JOB_UPDATE (`0x20`). Cancellation is cooperative; disconnect requests
cancellation of every running session job and aborts in-progress OTA.

## Screenshot and unified media

The 16-byte media description is:

```text
x:u16, y:u16, width:u16, height:u16
stride:u32, format:u16, quality:u16
```

SCREEN OPEN supplies a requested description. OPENED returns the negotiated
description plus `total_size:u32`, with a nonzero stream ID. READ uses that ID
and `offset:u32, maximum:u16, reserved:u16=0`; DATA returns
`offset:u32, total_size:u32, data[]`. The final DATA sets STREAM_END and appends
whole-object IEEE CRC32:u32 after its bytes. All reads are sequential and must
match the opening session/stream ID; CLOSE uses the same ID. The provider freezes
the snapshot from begin until end; stale IDs cannot read a newer image.

A capture is limited to 16 MiB, 30 seconds idle, and 600 seconds total. Control
chunks are at most 768 bytes; data uses its frame payload budget. Close, timeout
or owner-session loss releases the backend. The host validates size, offset,
stream ID, terminal boundary and final CRC and closes even on cancellation or
invalid responses. Gateway snapshots prefer data, fall back to control when no
data link exists, and accept `path=auto|control|data`. A forced control capture
requires any active screen mirror to stop. It never starts a continuous stream
on a weak link. Push-only providers retain the data mirror screenshot path.

SCREEN, IMAGE and AUDIO share MIRROR_START/MIRROR_STOP and DATA types.

The returned nonzero `stream_id` identifies the transfer task within its data
session. Media CREDIT and MIRROR_STOP must carry that exact ID; the device also
checks the owning session. Stale credits or a stop for another task cannot alter
the current stream. Reconnecting creates a new session and requires a new start.
MIRROR_START appends `fps:u16, reserved:u16` to the description. It is always
off after boot and link loss. Each active channel owns one bounded latest
chunk. A newer application submission overwrites an unsent chunk and
increments the dropped counter instead of creating backlog.

When SCREEN has a registered pull backend, MIRROR_START reuses that backend.
Raw RGB565/RGB888 frames are read as whole-scanline tiles no larger than
`CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES`; each tile description carries its
absolute `y` and tile `height`. All tiles in one frame share `frame_id`, while
the envelope `stream_id` remains the stable value negotiated by MIRROR_STATE.
This path does not allocate a second full framebuffer.

Unsolicited media DATA uses:

```text
monotonic_us:u64, frame_id:u32, dropped:u32
flags:u16, data_size:u16, description[16], data[data_size]
```

Each media channel has independent byte credit. CONTROL/EVENT responses are
scheduled before media, preventing congestion from starving control. The PC
Hub receives one physical stream and fans it out to bounded local queues.

## TCP pairing

USB is always auth mode 0. With `CONFIG_ESP_IRIS_TCP_PAIRING`, the device
stores a random 32-byte token in the existing `esp_iris` NVS namespace and
advertises auth mode 1 plus a fresh 32-byte challenge in HELLO. The token is
never sent on the link.

The TCP HELLO_ACK is `binding[17] || client_nonce[16] || hmac_sha256[32]`,
where binding is `role:u8 || owner_id[16]`. The HMAC key is the token and its
message is:

```text
"ESP-Iris-auth-0.2" || device_id[16] || boot_id:u64 || session_id:u32
|| challenge[32] || client_nonce[16] || binding[17]
```

The device uses PSA Crypto, constant-time comparison, a fresh challenge per
link session and a bounded nonblocking retry delay after failure. Only a successful
AUTH_RESULT makes the session ready; before that, every frame except
HELLO_ACK is discarded without reaching status, crash, RPC, media or OTA
handlers. Token get/rotate is intended for a product-owned secure
provisioning surface; Iris never logs the token.

## OTA and recovery

OTA BEGIN payload is:

```text
total_size:u32, sha256[32], project_len:u8, version_len:u8
reserved:u16, project[project_len], version[version_len]
```

The device accepts only the ESP-IDF-selected non-running app partition and
rejects oversized images. BEGIN_RESPONSE returns
`job_id:u32, total_size:u32, chunk_max:u16, label_len:u8, label[]`. The label
preserves up to 16 bytes of the ESP-IDF partition name; STATUS uses the same
bound. DATA is
`offset:u32, bytes[]` and must be strictly sequential. Every chunk updates a
PSA SHA-256 operation and `esp_ota_write`. END requires exact byte count,
full-image SHA match, a valid ESP-IDF image, exact agreement with the
project/version metadata supplied in BEGIN, and a successful recovery adapter
before selecting the boot partition. When
`CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH` is enabled, END additionally
requires the image project name to equal the running firmware project name.
The Gateway honors the advertised capability before recovery entry or direct
OTA. The option defaults off. The default preparation hook succeeds for
standard ESP-IDF OTA. Products that need retained-firmware metadata override
it; a product hook failure prevents boot-slot selection.
CANCEL, job cancellation, disconnect or any error calls `esp_ota_abort`.

The reference writer prepares only the declared image range in BEGIN, allowing
the Flash driver to batch erases before DATA arrives. This runs on the service
worker; status and cancellation remain available on the protocol task. Hosts
wait for each DATA response before sending the next block; protocol 0.2 does
not negotiate a pipelined Flash-write window. Hosts
allow up to 120 seconds for BEGIN (including full-partition erases),
while DATA retains its normal request timeout. Cancellation is cooperative and
releases the OTA handle after the current Flash operation returns.

STATUS has an empty request and returns
`job_id:u32, total_size:u32, received:u32, progress_permille:u16, active:u8,
label_len:u8, result:i32, label[]`. A host may use it after a response timeout
to determine whether the last chunk was committed and resume at the exact
reported offset.

`esp_iris_platform_select_ota_target()` lets a recovery image avoid the
retained last-known-good slot. `esp_iris_platform_prepare_ota()` records
last-known-good/target metadata without teaching Iris a custom partition
layout. `esp_iris_mark_healthy()` remains gated by product acceptance. RESTART
records planned intent and schedules a device-owned restart. The delay is clamped
to at least 100 ms; values above 60,000 ms are rejected. Repeated requests retain
the earliest deadline. Once the active service callback finishes, both protocol
TX queues may drain for up to 100 ms beyond that deadline; a stalled receiver
cannot defer restart indefinitely. Disconnect preserves the action, while an
explicit Iris stop cancels it. Factory, NVS, coredump and crash-evidence partitions
are never OTA targets. Crash collection
is an independent evidence workflow; the Gateway does not infer that a crash
was caused by an OTA operation.

After END verifies the image and successfully selects its boot partition,
Iris calls `esp_iris_platform_ota_committed()`. Its weak default does nothing,
preserving host-controlled restarts for existing products. A retained
Recovery writer may override it to schedule its own delayed restart, so a
lost END response or disconnected host cannot leave the committed image
waiting to boot. The hook is never called for a failed or cancelled OTA.

## Compatibility vectors

[`golden_vectors.json`](golden_vectors.json) is the normative byte-level 0.2
compatibility set. Device C and PC Python codec tests consume the same file.
Any intentional envelope change requires a new negotiated protocol version;
Historical 0.1 vectors remain unchanged in `archive/0.1/`.

## System Update

System Update is a recovery-only, policy-controlled multi-image transport. It is
advertised with `CAP_SYSTEM_UPDATE` (capability bit 15) only after a product
backend has registered. The generic ESP-Iris component never treats a target
offset as permission to write Flash. The backend parses the manifest,
optionally authenticates it according to product policy, cross-checks every
component descriptor, and implements the Flash policy.

The read-only inventory provider is independent. A normal or recovery image
advertises `CAP_SYSTEM_INVENTORY` (capability bit 16) only after that provider
registers. This lets the Gateway verify actual Flash after booting the normal
application without leaving an update writer reachable there.

All update messages use channel 9. A 16-byte nonzero operation ID identifies
one plan across every request.

BEGIN (`0x01`) is:

```text
operation_id[16], manifest_size:u16, signature_size:u16,
component_count:u8, flags:u8, reserved:u16, manifest_sha256[32],
manifest[manifest_size], signature[signature_size]
```

The manifest and optional signature are bounded by Kconfig. Manifest v1 does
not negotiate these limits: the reference host accepts manifests up to 3072
bytes and up to eight components, while device builds may have lower limits
(defaults: 2048 bytes and four components). Products using the larger capacity
must configure their Recovery accordingly before installing those bundles.
Manifest v1 does not contain a product-specific source-layout allowlist: the
product backend must validate source-to-target compatibility before accepting
any destructive component write. The complete target partition-table hash remains mandatory
as an explicit layout precondition and for post-reboot inventory validation. A zero
`signature_size` represents an unsigned update. ESP-Iris always verifies the
manifest SHA-256; the product backend decides whether a signature is required
and enforces the product Flash policy.
BEGIN_RESPONSE (`0x02`) is `operation_id[16], job_id:u32, chunk_max:u16,
component_count:u8, flags:u8`.

COMPONENT_BEGIN (`0x03`) is:

```text
operation_id[16], component_id:u8, kind:u8, flags:u16,
target_offset:u32, total_size:u32, sha256[32]
```

Defined kinds are bootloader `1`, partition table `2`, application `3`,
recovery `4`, and product data `5`. A backend may reject any kind, including
recovery self-update. COMPONENT_BEGIN_RESPONSE (`0x04`) returns
`operation_id[16], component_id:u8, kind:u8, chunk_max:u16, total_size:u32`.
The reference bundle builder requires a Recovery component to declare its
complete sector-aligned protected size and pads the input image with erased
bytes to that size before hashing. Recovery is the bundle's only component;
the partition table is not transported or committed. The mandatory
`target_layout_sha256` field instead states the exact source-layout
precondition. A Recovery self-update cannot be mixed with normal application,
bootloader, partition-table, or data updates.

DATA (`0x05`) is `operation_id[16], component_id:u8, reserved:u8,
reserved:u16, offset:u32, bytes[]`. Offsets are strictly sequential. Each
accepted chunk is hashed before it reaches the backend. DATA_RESPONSE (`0x06`)
is `operation_id[16], component_id:u8, reserved:u8, progress_permille:u16,
committed_offset:u32`.

The reference device supports configured DATA blocks up to 3968 bytes, keeping
the 24-byte DATA header within the 4000-byte payload limit. Hosts must use the
smaller limit returned by BEGIN and COMPONENT_BEGIN, including older writers
that advertise 1024 or 2048 bytes. DATA remains stop-and-wait: acknowledge the
committed offset before sending another chunk; a larger block does not enable
concurrent service requests.

COMPONENT_END (`0x07`) is `operation_id[16], component_id:u8, reserved[3]`.
It succeeds only after the exact byte count, streamed SHA-256, and backend
readback validation all agree. COMPONENT_END_RESPONSE (`0x08`) is
`operation_id[16], component_id:u8, completed_count:u8, reserved:u16,
result:i32`.

COMMIT (`0x09`) contains only the operation ID. The backend should retain
bootloader and partition-table bytes in internal RAM until this point, write
and read back the bootloader, prepare fixed-address boot selection metadata,
and replace the partition table last. COMMIT_RESPONSE (`0x0a`) is
`operation_id[16], job_id:u32, result:i32`. A successful backend must keep the
link alive long enough to queue the response and may then schedule a restart.

CANCEL (`0x0b`) aborts an uncommitted operation. STATUS (`0x0c`) has an empty
payload and STATUS_RESPONSE (`0x0d`) is:

```text
operation_id[16], job_id:u32, phase:u8, component_count:u8,
completed_count:u8, active_component_id:u8, received:u32, total:u32,
result:i32
```

Disconnect aborts a prepared or receiving operation. A partially written
future application is not selected; sensitive components must not have been
written before COMMIT. Response timeouts can be reconciled through STATUS.

INVENTORY (`0x0e`) has an empty payload. INVENTORY_RESPONSE (`0x0f`) is:

```text
flags:u32, layout_version:u32, bootloader_sha256[32],
partition_table_sha256[32], last_operation_id[16], last_result:i32
```

Flag bits 0, 1, and 2 mark the respective bootloader hash, partition-table
hash, and last-operation fields as valid. Hashes must be calculated from the
current Flash contents, not copied from sysmeta. Bootloader and partition-table
hashes cover exact product-defined protected ranges including erased-byte
(`0xff`) padding; the bundle builder uses the same ranges. For the standard
layout this is the bootloader start through the byte before the partition
table, plus the complete 4 KiB partition-table sector. Source compatibility is
owned by the product backend. The Gateway verifies target inventory, operation
ID, application identity, and product health after reboot.

## Local TCP discovery (outside the wire envelope)

TCP products may publish the DNS-SD service `_esp-iris._tcp.local.`. Each
instance name is unique per device and its SRV port is the raw ESP-Iris TCP
port. TXT records contain `device_id`, `protocol`, `transport`, `pairing`,
`mode`, `port`, and optional `data_port`. For 0.2, `transport=tcp`, `protocol=2`, and
`pairing` is either `none` or `hmac`; `device_id` is the same 32-character
lowercase identity later returned by HELLO. The Gateway rejects advertisements
whose TXT identity and authenticated/session HELLO identity differ.

mDNS is an unauthenticated local-link discovery hint. It never carries the
pairing token and does not change the binary protocol or its golden vectors.
# Additive firmware compatibility metadata (IRIS-P03)

## Gateway write preflight

OTA and System Update require a live explicit `normal` or `recovery` role and
`chip_target` matching the artifact before an enter-Recovery RPC or a writer is
called. Current artifact support is ESP32-S31 (`chip_id=0x20`); this does not
expand target support. `execution_mode=application` means use the current writer
without a Recovery transition; it still requires an explicit role and chip.

Callers may supply a `compatibility` object with `chip_target`,
`product_contract`, `board_id`, `layout_id`, and/or `recovery_abi`. Strings must
contain 1..64 UTF-8 bytes; the ABI must be an integer 1..65535. Unknown keys,
missing device declarations, and mismatches are rejected. An absent expectation
does not invent product metadata: independent examples may omit product fields.
All nonempty declarations observed in normal firmware are retained and checked
again in Recovery before writing, even without a caller expectation. The actual
System Update source-layout hash remains separately checked against its bundle.

- `POST /v2/devices/{id}/ota`: JSON field `compatibility` alongside `artifact_id`.
- `POST /v2/devices/{id}/system-update`: JSON-encoded header `X-Iris-Compatibility`
  alongside the binary archive body.
- Both CLI commands accept `--compatibility-json '{"chip_target":"esp32s31"}'`.

The normalized expectation is persisted in operation parameters and included in
the operation request fingerprint. Reusing an operation ID with different
expectations returns conflict rather than reusing or running another write.
These expectations select a compatible device; they do not authenticate the
artifact or replace signed bundle policies.

0.2 firmware without a product role/chip declaration can be observed but cannot be updated
through these generic writers. Migrate it through the product's provisioning or
recovery procedure defined by that product, install firmware declaring
the contract, then resume the normal install workflow. There is no project-name
fallback or implicit legacy write bypass. Rebuild the supplied examples/fixtures
to obtain their explicit normal/Recovery role declarations.

HELLO uses wire version 2. The following TLVs describe product compatibility. Integers are little endian. Unknown optional
tags are ignored. Identity strings are UTF-8, at most 64 bytes, without a NUL.

| Tag | Value | Meaning |
| --- | --- | --- |
| 0x0e | u8 | Firmware role: 0 unknown, 1 normal application, 2 recovery |
| 0x0f | string | Product compatibility contract identifier |
| 0x10 | string | ESP-IDF chip target, e.g. esp32s31 |
| 0x11 | string | Board compatibility identifier |
| 0x12 | u16 | Recovery ABI version; 0 unspecified |
| 0x13 | u64 | Required host features; currently no bits defined |
| 0x14 | u32 | Product health observation timeout, 1000..600000 ms |
| 0x15 | string | Partition layout contract identifier |
| 0x16 | 6 bytes | Factory eFuse Base MAC (`HARDWARE_MAC`) |

Firmware emits its explicit Kconfig declarations (`ESP_IRIS_FIRMWARE_ROLE`,
`ESP_IRIS_PRODUCT_CONTRACT`, `ESP_IRIS_BOARD_ID`, `ESP_IRIS_RECOVERY_ABI`,
`ESP_IRIS_HEALTH_TIMEOUT_MS`, `ESP_IRIS_LAYOUT_ID`) and `IDF_TARGET`. Invalid
oversized identifiers fail the firmware build. The current firmware emits zero
required-feature bits. A new host rejects any unknown required bits, invalid
role, numeric lengths or health deadline before HELLO_ACK; it must not issue
mutating requests to such a peer. Introducing a requirement old hosts cannot
understand requires a protocol version bump, not merely this additive tag.

An absent role is **unknown**, regardless of project/version/USB product names.
Product installers require an explicit matching role/contract. Old 0.1 peers
are rejected before services; there is no legacy adapter.
The role is authoritative only after HELLO; USB discovery hints are provisional.
Missing string/ABI fields mean unspecified; the unspecified health default is 45000 ms.
These declarations do not authenticate a device or prove its flash layout. Match
the selected product/chip/board/Recovery ABI contract and verify the actual
system inventory/layout hash before writes. Existing `AUTH_MODE` and capability
bits continue to describe transport security and available services; this change
does not add encryption, signing, or new target support.

## Session replay and RPC retry boundary (IRIS-P04)

`CAP_SESSION_REOPEN` (bit 17) negotiates `HELLO_ACK` flag `NEW_SESSION`
(bit 5). A new host session sends this flag once it sees the capability, even
on an initially provisional link. The device authenticates the ACK first,
ends the previous logical session, generates a fresh nonzero Session ID,
and sends HELLO. The host waits for this new HELLO before sending a normal
ACK and advertising readiness. A duplicate NEW_SESSION for the old Session ID
is discarded. A normal ACK for the current session is idempotent. 0.2 firmware advertises this capability. This explicit handshake lets a
new USB Serial/JTAG host reopen a device whose physical USB connection stayed up.
It is not an authenticated-channel substitute; TCP pairing remains a separate
policy. Reopening invalidates objects owned by that session, while the other healthy
link retains its independently owned tasks.

Within a session, non-HELLO_ACK frames must advance their per-channel uint32
sequence using serial arithmetic: distance must be in [1, 2^31). The first
sequence may have any value; zero is valid after wrap. Duplicate/stale frames
are discarded before services and counted as invalid. Transport ordering is
required; reordered requests are not retried automatically.

RPC request IDs must be nonzero and advance by the same serial arithmetic.
The device retains one high-water mark for the whole session and rejects ALL
reused or older IDs with INVALID_STATE, including A, B, A and changed payloads
under the same ID. This is a bounded-memory rejection contract, not a response
cache: after a lost ACK the caller must reconcile product state, never assign
a new ID to automatically repeat a mutating RPC. IDs and frame sequences reset
only at a new session. This provides no exactly-once guarantee across session
reopen, power loss, or reboot. FILE/OTA have their own offset/receipt/status
protocols; transfer requests remain bound to their opening data session.

All transport configurations, including TCP-only firmware, keep a physical
connection provisional until a valid HELLO_ACK. The configured claim deadline
releases unhandshaken clients; an authenticated/acknowledged owner is exempt
from this provisional deadline. For USB Serial/JTAG, a rejected candidate keeps
the existing claim cooldown before it may compete again. This is a bounded
handshake lease, not a new application idle timeout.

## Slow-service execution and cancellation (IRIS-A02)

RPC callbacks, CONTROL job-cancel callbacks, and OTA BEGIN/DATA/END/CANCEL run
on one lazy `iris-service` worker. A single owned request slot bounds RAM and
queue latency; saturation returns INVALID_STATE before executing another
request. The protocol worker continues PING, TIME_SYNC, STATUS and job queries.
Responses return through a separate owned buffer and are encoded by the
protocol task with its current channel sequence. Session changes discard old
completions and defer cleanup until the active callback/flash call returns.

`CONFIG_ESP_IRIS_SERVICE_STACK_SIZE` defaults to 6144 bytes. The executor owns
one private runtime buffer and one task while slow work is active. Once the
completion has drained and the executor remains idle for
`CONFIG_ESP_IRIS_SERVICE_IDLE_TIMEOUT_MS` (1000 ms by default), the worker and
private buffer are released and recreated for the next request. Registration
contexts must remain valid until unregister succeeds. Unregister rejects while
work is active. A subsequent start rejects until deferred cleanup completes.

A deadline already expired before callback dispatch prevents invocation. A
callback that exceeds its deadline returns TIMEOUT with no response body, but
its side effects are not rolled back. Arbitrary application C callbacks cannot
be forcibly cancelled safely. A non-returning callback occupies the sole service
slot; CONTROL stays available and additional service work is rejected. Long
product work should expose a job and poll `esp_iris_job_cancel_requested()`.

Cancellation is cooperative: CONTROL CANCEL marks the job immediately; its
callback runs on the service worker after the current step. OTA CANCEL while a
flash step is running acknowledges cancellation intent without STREAM_END.
The current flash API call may finish before abort. OTA STATUS and the public
OTA status API expose the last completed snapshot during work. END checks
cancellation before boot selection; once boot selection starts, a late cancel
cannot promise rollback. Bytes already written, completed callbacks, and
platform preparation metadata may remain. No timeout or disconnect retries a
mutating step automatically. Full device restart is still needed for a hung
callback to release its occupied slot; forcibly deleting its task would risk
corrupting locks and flash ownership.

SYSTEM_UPDATE backend prepare/write/end/commit/cancel steps use the same
bounded executor. SYSTEM_UPDATE STATUS reads a published status and Job ID
snapshot on the protocol worker. A concurrent SYSTEM_UPDATE CANCEL validates
its Operation ID against that snapshot and acknowledges intent using STATUS;
it does not claim STREAM_END. Commit checks cancellation immediately before
entering the backend; an already-entered product commit cannot be rolled back.
Successful `esp_iris_stop()` waits for deferred service cleanup. If a callback
still owns work after the bounded stop deadline, stop returns TIMEOUT and
contexts must remain valid; unregister/start reject until work is released.
Completion draining retains unsent responses under TX backpressure and claims
the slot exclusively against concurrent deferred cleanup.


## Reserved optional RPC service profiles (v1)

These profiles reserve service/method identifiers without changing the generic
CONTROL REQUEST/RESPONSE envelope. Implementations opt in by registering the
profile handler. The generic RPC capability does not imply profile support;
unsupported methods return the normal RPC error. C consumers use
`esp_iris_service_profiles.h`; host adapters use `service_profiles.py`.

| Profile | Service | Method | Request | Successful response |
| --- | --- | --- | --- | --- |
| `pointer/v1` | `0x1001` | `1` | 12-byte pointer sample | 12-byte accepted sample |
| `enter-recovery/v1` | `0x7fff` | `2` | Empty | Empty, followed by restart |

The pointer sample is little-endian `<BBhhHI>`: phase u8 (0 begin, 1 move,
2 end), reserved u8=0, x i16, y i16, reserved u16=0, sequence u32.
Coordinates are display pixels, with origin at the top left. The host obtains
full-screen width and height from SCREEN MEDIA.OPEN with the default description
and closes the media handle after reading its description. Normalized host points
0..10000 map to 0..width-1 and 0..height-1. Dimensions must fit positive signed
16-bit coordinates. A product must not substitute a fixed panel size in the
Gateway. Response coordinates may reflect device-side clamping.

Enter-recovery schedules a restart into the firmware's declared recovery role;
a successful RPC is acceptance, not proof of arrival. A host confirms the same
Device ID, a new Boot ID and recovery role after reconnect. A lost response may
be followed by observation but must not cause automatic replay of the RPC.
The profile specifies no Mosaico partition, product version or persistent ABI.

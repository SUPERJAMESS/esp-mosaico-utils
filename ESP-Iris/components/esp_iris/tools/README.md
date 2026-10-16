# ESP-Iris Developer Gateway and Web Workbench

The Developer Gateway is the PC-side fleet hub for ESP-Iris. It supervises
multiple USB and TCP device endpoints concurrently, identifies devices by
stable `device_id`, exposes REST/WebSocket APIs, stores durable engineering
records, and serves the React Web Workbench. The `ctl` command-line client and
external agents use the same API concurrently with the Workbench.

Each physical device has one Gateway owner, with independently bound control
and data sessions. One Gateway can own many devices. Every Workbench, CLI follow command, or external
agent WebSocket receives an independent event stream. Device-changing
operations are serialized per device; observers do not compete for events.

The Gateway is source distributed with the ESP-IDF component. It is not
installed as a Python package.

When launched with project session options, discovery is passive, acquisition
is explicit, and only the
current owner reconnects a device. Local `/v2/project` APIs report ownership
and perform reserved, idempotent device transfers. Shared project Gateways use independent client leases:
CLI clients renew while their command runs, each foreground client retains its own
client, and each Web workbench event connection retains a client. With no
clients and no active work, the service shuts down after 10 idle seconds.
HTTP status queries do not retain clients; there is no stop endpoint. The
owner-pipe lifecycle is available for a directly supervised process. Each project
keeps separate records; API/capability compatibility governs reuse. Exact source matching is optional.
The standalone `esp_iris.py web` commands below retain their existing automatic
connection behavior and do not participate in project ownership coordination.

## Requirements

- Python 3.8 or newer
- Linux, macOS, or Windows; real-board validation currently focuses on Linux
- Node.js and npm when the Workbench must be built from source
- Exclusive console access while Gateway is attached; stock monitor works independently

Set one path for the rest of this guide:

```bash
# Source checkout
ESP_IRIS_COMPONENT_DIR=components/esp_iris

# Component Manager installation
# ESP_IRIS_COMPONENT_DIR=managed_components/lisir233__esp_iris
```

PowerShell users can set the equivalent `$env:ESP_IRIS_COMPONENT_DIR` value and
use `python` when it is the Python 3 launcher.

## Install the Gateway

Create an isolated environment and install the runtime dependencies:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r "$ESP_IRIS_COMPONENT_DIR/tools/requirements.lock"
```

`requirements.lock` is shared across supported Python releases. Its PEP 508
markers make pip select the validated packages for the active interpreter
(including the Python 3.8-compatible aiohttp and zeroconf versions). This
Gateway interpreter is independent of the interpreter required by ESP-IDF.

This checkout includes a prebuilt Workbench in `tools/frontend/dist`, which
the Gateway serves directly. Node.js and npm are not required to open it.
The component file rules retain this distribution and exclude `node_modules`.
To rebuild after changing frontend source or its dependency lock:

```bash
cd "$ESP_IRIS_COMPONENT_DIR/tools/frontend"
npm ci
npm run build
cd -
```

Commit the regenerated `dist` files together with frontend source changes.
CI rebuilds from `package-lock.json` and checks that the committed distribution
matches. The Gateway displays a fallback page when `dist/index.html` is absent.

## Evaluate without hardware

Demo mode creates virtual normal/recovery devices and exercises logs, RPC/jobs,
screen/input, media, OTA, restart, disconnect, crash evidence, and the
file browser with upload and metadata mutations:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --demo
```

Open `http://127.0.0.1:8443/`.

## Connect devices

USB and TCP options are repeatable and may be combined. For example, one
Gateway process can supervise an application CDC device, a USB Serial/JTAG
device, and two network devices at the same time:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --usb /dev/serial/by-id/usb-Espressif_ESP-Iris_A... \
  --usb-serial-jtag /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_B... \
  --tcp 192.0.2.21:19772 \
  --tcp 192.0.2.22:19772
```

Automatic application CDC and mDNS discovery are enabled when no endpoints are
specified. With explicit USB or TCP endpoints, enable additional discovery with
`--discover-usb` and/or `--discover-mdns`. A device may wait on several configured transports, but a
validated HELLO_ACK selects exactly one active session; it never maintains
simultaneous USB and TCP sessions.

### Automatic TCP discovery with mDNS

The default process browses `_esp-iris._tcp.local.` on multicast-capable local
networks and supervises every valid advertised endpoint. Disable this when the
host should connect only to explicit endpoints:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --no-discover-mdns
```

Discovered services must advertise protocol version 1, a 32-character device
ID, TCP port, firmware mode, and `none` or `hmac` pairing metadata. The HELLO
identity must match the advertised device ID. Pairing tokens are never read
from mDNS; supply the existing `--pairing-token` option when required. mDNS
does not cross routed subnets unless the network provides an mDNS reflector.

### Automatic application USB discovery

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web
```

The default process discovers all visible ESP-Iris application CDC devices,
listens on `127.0.0.1:8443`, and serves the API and Workbench.

### Select an application USB port

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --usb /dev/serial/by-id/usb-Espressif_ESP-Iris_...
```

An explicit USB port may be absent at startup. The API and Workbench still
start, and the port is retried until it can be resolved and exclusively owned.
`GET /v2/endpoints` reports waiting, ambiguous, or owned-elsewhere conditions.
Aliases and automatic discovery share one physical endpoint lock and session.

Explicit `--usb` selection supports custom application VID/PID/product strings;
automatic discovery retains its ESP-Iris descriptor filter. Every connection
must complete the ESP-Iris handshake. Espressif `303A:1001` still requires the
Serial/JTAG opt-in below, and `303A:0020` ROM download ports remain reserved for
local ROM operation executor.

### Console commands from the Workbench

The log view hides whitespace-only records by default. Console protocol responses
start with CR/LF to separate their marker from unfinished application output;
these separators are retained in capture/history but should not fill the view
with empty `raw` rows. **Show blank lines** restores them in the view. Whitespace
inside a nonempty message is unchanged.

The device log panel includes a command input. Enter submits one text line;
Up/Down recalls the last 32 commands in that device view. `iris help` and
`iris status` work with the built-in 0.2 console; application commands depend
on the product's existing console/REPL. The **Enter command** button focuses
this input. Offline, Observe-mode and data-only devices cannot send text.

`POST /v2/devices/{device_id}/console` accepts `{"line":"iris status"}` and
writes UTF-8 text plus LF through the active console control link, sharing the
Iris record write lock. It no longer invokes a `console.execute` RPC or returns
a Job ID. `console.sent=true` and `completion="unconfirmed"` mean the host write
completed; firmware output remains in the ordinary log stream, without a
synthetic command-success result. The operation's success describes sending,
not command execution. `X-Operation-ID` deduplicates repeated submissions.
Failed/partial writes are never automatically replayed onto a new session.

Lines are limited to 255 UTF-8 bytes and exclude control characters and line
separators. Machine records (`iris @…`) and `iris hello` remain owned by Gateway
so manual input cannot replace its protocol/session negotiation. No extra RPC
handler or second serial reader is needed. Products using an external ESP-IDF
REPL should use the current `esp_iris_console_register_commands()` callback:
it yields for up to one second when the single-line queue is occupied, instead
of rejecting a command racing a status poll. The custom
`esp_iris_console_submit()` API remains nonblocking; its input owner must handle
`ESP_ERR_TIMEOUT`. This is line input,
not a terminal emulator or a channel for Ctrl-C/debugger control sequences.

### Device state and local ROM operations

Device and endpoint inventories expose one `state`: `offline`, `discovered`, `connecting`,
`idle`, `busy`, or `needs_recovery`. `owner_session_id` and `firmware_mode`
(`normal`, `recovery`, `rom`, `unknown`) are independent fields. `busy_reasons`
identifies active operations, jobs and mirrors. Log viewers and client leases
alone do not make a device busy. An active operation remains busy across its
expected USB disconnect/re-enumeration; only live ROM descriptors justify a
`needs_recovery` diagnosis, not an HTTP timeout or a failed operation record.

`discovered` means the port is present but has no live Iris session. Only an
active handshake/connection attempt is `connecting`; cached control/data link
fields are cleared on disconnection. Historical firmware and metrics remain
marked as cached, never as evidence of a live connection.

The device page's **Connect devices** picker passively lists High-Speed USB,
USB Serial/JTAG and USB UART adapters. Select **Connect to this project**, with
the firmware's console baud rate for UART (default 115200; custom rates such as
74880 are supported). Manual serial paths and `tcp:host:port` with an optional
pairing token are also supported. USB console/data roles remain explicit;
data interfaces are independently verified and grouped by live identity.
**Disconnect selected device** releases all of that device's links and stops
its reconnect supervisors, preserving history. Busy devices must finish or stop
their active operation before release. Discovery alone never acquires a port.

Host-side ROM probes and Recovery installation use the ordinary operation
queue/history (`host.probe`, `host.recovery`). The local CLI publishes a private,
one-use request file and submits its ID to `POST /v2/host-operations`. Executable
commands and environment values are never accepted in the HTTP body or saved
in public operation parameters. Browser-origin and remote submissions are
rejected. Results are read through the existing operation status endpoint.

The operation blocks new work on all involved device/endpoint resources,
preserves available crash evidence, detaches the Iris connection, and starts a
separate worker. That worker owns the physical USB locks for the entire
foreground command lifetime. Closing the CLI or losing the Gateway does not
kill the writer or release its locks. A CLI wait timeout reports the operation
ID; it does not interrupt flash. Commands must wait for all their writers to
exit, rather than launching detached background work.

After writing, the Gateway reattaches and verifies the selected device, a new
Boot ID, Recovery mode, expected version/MAC and OTA capability. A failed write
or verification stays in operation history. Completed processes leave no
renewable reservation or token requiring manual cleanup. On Gateway restart,
interrupted writes are recorded as `outcome_unknown`, never replayed; still-live
workers remain visible as busy and keep their process-owned physical locks.

The former maintenance lease API, CLI commands and persisted lease table are
removed. All participating host tools must use this implementation; mixed
versions and resuming old lease workflows are not supported.

Application CDC0 carries text logs and printable control records. CDC1 carries
bulk data. Stock monitor needs no Iris extension; release Gateway's console
ownership before attaching monitor. Flash/reset behavior follows the hardware
interface and the application's bootloader configuration.

### Select USB Serial/JTAG

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --usb-serial-jtag /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
```

Automatic USB Serial/JTAG probing is intentionally disabled because its fixed
descriptor does not identify the running firmware. Use the explicit option
above or opt in with `--discover-usb-serial-jtag`. Stop the Gateway before
flashing or monitoring through the same endpoint.

### Connect raw TCP explicitly

The device application must first create its network interface. Then select its
TCP endpoint in the Gateway/Workbench. Start with the
[`tcp_wifi`](../examples/tcp_wifi/README.md) or
[`tcp_pairing`](../examples/tcp_pairing/README.md) example.

### Select UART and reset explicitly

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --uart /dev/serial/by-id/<adapter> --uart-baudrate 115200
```

Match the product's `CONFIG_ESP_CONSOLE_UART_BAUDRATE` (for example 74880 on
ESP32-C2 with a 26 MHz crystal). UART and Serial/JTAG support RPC, jobs, input
and bounded screenshots; continuous media, files and firmware require data.
Gateway opens and reconnects without resetting. Its hardware-reset command/API
starts capture before applying DTR/RTS or Serial/JTAG run/ROM sequences. Raw
bytes before HELLO retain capture offsets and an unknown boot identity; later
association does not relabel old bytes as belonging to a new boot.

## Access and authentication

Loopback clients are authentication-free by default. Use
`--require-local-auth` to exercise the login flow locally:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --require-local-auth
```

A new Gateway initializes the developer password to `espressif`. Override that
first-run value with `ESP_IRIS_DEVELOPER_PASSWORD` or `--password-file`, then
change it from System Settings. Do not expose the default password outside an
isolated development environment.

To serve a trusted development LAN explicitly:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --listen 0.0.0.0 --port 8443
```

Plain HTTP does not protect passwords, Agent Tokens, logs, media, or commands.
Enable `--tls` for a generated local certificate, or provide `--tls-cert` and
`--tls-key`. The Gateway prints the generated certificate SHA-256 fingerprint.

Do not rely on `X-Forwarded-For` to turn a remote client into a loopback client;
the Gateway authorizes the actual TCP peer.

## Workbench pages

| Page | Purpose |
| --- | --- |
| Overview | Device identity, connection, firmware, health, and current status |
| Logs | Live and retained device logs |
| Workspace | RPC, console, screen/input, media, firmware, OTA, and restart actions |
| Files | Product-registered logical volumes, paginated directories, streamed upload/download, rename, mkdir, and safe delete |
| Operations | Durable long-running operation state and progress |
| Records | Sessions, evidence, artifacts, and retained history |
| Settings | Access mode, credentials, tokens, TLS, and system configuration |

The interactive OpenAPI view is available at `/docs`; the machine-readable
contract is `/v2/openapi.json`; metrics are exposed at `/v2/metrics`.

### File API

The file endpoints address only product-registered logical volumes:

| Method and path | Behavior |
| --- | --- |
| `GET /v2/devices/{id}/files/volumes` | Volume capabilities and transfer limits |
| `GET /v2/devices/{id}/files` | Paginated directory list |
| `GET /v2/devices/{id}/files/stat` | File or directory metadata and opaque ETag |
| `GET /v2/devices/{id}/file` | Stream download, including HTTP Range |
| `PUT /v2/devices/{id}/file` | Stream create or atomic replacement |
| `POST /v2/devices/{id}/directories` | Create one directory |
| `POST /v2/devices/{id}/file-rename` | Same-volume rename without overwrite |
| `DELETE /v2/devices/{id}/file` | Delete a file or empty directory |

Upload requires `Content-Length` and is limited to 32 MiB by the Gateway. Use
`overwrite=true` with `If-Match` for replacement. The Gateway forwards request
chunks directly to the device, records only path/size/hash/result metadata,
and serializes every mutation with other writes to that device. Agent Tokens
need `files.read`, `files.write`, or `files.delete` as appropriate; new tokens
default to `files.read`.

## Command-line client

The `ctl` client talks to the same Gateway API as the Workbench. It can run at
the same time as one or more Workbench and agent clients:

```bash
IRIS="$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py"

python "$IRIS" ctl devices
python "$IRIS" ctl status DEVICE_ID
python "$IRIS" ctl logs --device DEVICE_ID --follow
python "$IRIS" ctl rpc DEVICE_ID system.info --params '{}'
python "$IRIS" ctl jobs DEVICE_ID JOB_ID
python "$IRIS" ctl cancel DEVICE_ID JOB_ID
python "$IRIS" ctl screenshot DEVICE_ID device.png
python "$IRIS" ctl mirror DEVICE_ID start --fps 5
python "$IRIS" ctl mirror DEVICE_ID stop
python "$IRIS" ctl restart DEVICE_ID
python "$IRIS" ctl firmware-add build/app.bin
python "$IRIS" ctl ota DEVICE_ID build/app.bin
python "$IRIS" ctl ota DEVICE_ID build/app.bin --validation-mode version
python "$IRIS" ctl system-update DEVICE_ID release.irisfw --wait
python "$IRIS" ctl operation-status OPERATION_ID
python "$IRIS" ctl operation-watch OPERATION_ID
python "$IRIS" ctl mode observe
```

OTA validates the firmware identity again after the device reconnects. The
default `elf_sha256` mode compares the ELF SHA-256 embedded in the uploaded BIN
with the running device's reported firmware SHA-256. Use
`--validation-mode version` only for compatibility with workflows that must
compare the project version string instead. The Workbench OTA dialog and the
REST request field `validation_mode` expose the same choice; supported values
are `elf_sha256` and `version`.

System Update supports signed and explicitly unsigned bundles. For an unsigned
development bundle, omit the manifest `signature` object and all key options:

```bash
python "$IRIS" bundle build manifest-template.json \
  --component-root build/system-update \
  --output release.irisfw
python "$IRIS" bundle inspect release.irisfw
python "$IRIS" web
```

For a signed deployment, start the Gateway with a pinned ECDSA P-256 trust key.
The private signing key is used only by the offline bundle command and is never
passed to the Gateway or device:

```bash
python "$IRIS" bundle build manifest-template.json \
  --component-root build/system-update \
  --signing-key /private/release-signing-key.pem \
  --signing-key-password-file /private/release-signing-key.password \
  --output release.irisfw
python "$IRIS" bundle inspect release.irisfw \
  --trust-key release-public-key.pem
python "$IRIS" web --system-update-trust-key release-public-key.pem
```

The Gateway archives the exact `.irisfw`, preserves a valid coredump before
recovery entry, verifies the source layout through device inventory, streams
the components in manifest order, and accepts success only after a new
healthy boot reports matching application, bootloader, partition-table and
operation identities. When a trust key is configured, unsigned bundles are
rejected. Without a trust key, signed bundles are rejected and only explicitly
unsigned bundles are accepted.

Encrypted signing keys are supported through the optional password-file flag;
the password is never accepted as a command-line value. Keep the private key
and its password outside the Gateway host in the release-signing environment.

The bundle builder pads the partition-table image to its full 4 KiB sector.
When a bootloader is present it must be accompanied by the target partition
table, and the builder pads it with `0xff` through the byte before that table.
The product inventory provider must hash those same logical protected ranges;
on products with Flash encryption it is responsible for choosing the matching
plaintext/readback semantics. Normal firmware exposes only read-only inventory,
while recovery additionally registers the product write backend.

Use `ctl --json` for stable machine-readable output. A remote Gateway or local
Gateway started with `--require-local-auth` requires a login or named Agent
Token. Pass Agent Tokens through a protected file rather than a command-line
argument:

```bash
export ESP_IRIS_AGENT_TOKEN_FILE=/private/path/agent.token
python "$IRIS" ctl --json devices
```

Profiles store the Gateway URL, browser-style session, optional CA path, and
optional certificate fingerprint. Configure a profile with:

```bash
python "$IRIS" ctl --profile lab-a \
  --url https://192.0.2.10:8443 --ca gateway.crt profile --make-default
```

## Runtime and storage model

Project clients keep diagnostics in the selected application's `.gateway/`:
`state/gateway.sqlite3`, `state/events/`, `state/artifacts/` and `gateway.log`.
The directory contains a Git ignore file and is independent of `build/`.
Connection records, launch locks and cross-project device ownership remain in
the host's `project-sessions/` and `ownership/` directories. Passwords, tokens,
host identity and TLS files stay in a separate host-local credentials store.
Moving or copying a project preserves its history without copying a live
Gateway connection or its authentication credentials.

For read-only or network-mounted projects, pass `mosaico.py iris run
--gateway-data-dir /local/disk/project-gateway`. This location is remembered for
that project on this host and cannot be assigned to a second project. The CLI
prints the actual data location; `/v2/health` also reports it. Stop the Gateway
before changing this location.

Existing 0.2 data is relocated explicitly while the Gateway is stopped:

Before upgrading, stop every Gateway using the data and make a separate backup
of the complete state directory, including any SQLite WAL/SHM files, logs,
artifacts and event segments. Back up the host credentials directory too if it
is separate. Keep these backups outside `.gateway/` and outside retention paths.

```bash
python "$IRIS" storage --state-dir /host/project-sessions/PROJECT/state \
  --migrate-to /application/.gateway/state
```

The copy is validated and published atomically. It omits credentials and TLS
keys; the original host directory continues to supply authentication. An
interrupted copy can be retried, and an existing destination is never replaced.
The source is retained, but it continues to serve as the live credentials store:
starting the new Gateway can upgrade its schema and update authentication state.
Use the separate pre-upgrade backup for rollback, not that live source.

Opening a schema 200 or 201 database upgrades it in place to schema 202. This
also applies to standalone `web --state-dir` and the `storage` inspection command;
reverting the code alone does not downgrade the database. To roll back, stop the
new Gateway, archive its current data, restore the complete pre-upgrade state
and credentials directories, and start the previous code against those restored
directories. Records and credential changes made after the backup remain only in
the archived new state; rollback does not merge or replay them. Keep the retained
source while it is the live credentials store. Independent pre-upgrade backups
still consume disk space; archive or delete them explicitly once the rollback
window has closed.

- One Gateway supervises many USB/TCP endpoints concurrently and indexes their
  sessions and events by stable `device_id`.
- Each physical device has at most one active session, even if more than one
  configured endpoint resolves to that device.
- Each WebSocket client receives its own live event queue and can resume from a
  retained event cursor. Workbench, CLI, and agents therefore observe without
  consuming one another's events.
- A stable `device_id` joins normal and recovery firmware into one device
  history; `boot_id` identifies a boot and `session_id` identifies a link.
- Device writes are serialized per device. OTA/recovery holds that device's
  write lane without blocking observation of it or other devices.
- `operation_id` makes long-running Gateway operations queryable and
  idempotent; Gateway restart does not replay device writes.
- Observe mode blocks business device requests while protocol housekeeping and
  device-pushed events continue.
- SQLite stores devices, sessions, operations, audit, token metadata, and log
  indexes. Raw logs rotate; explicitly saved artifacts and structured evidence
  remain durable.
- A newly connected Workbench, CLI follower, or agent receives recent stored
  events before live events, preserving continuity across reconnects.

## Troubleshooting

### Diagnostic retention and storage failures

Gateway resource admission is configured with `web --resource-policy limits.json`
and persisted per Gateway. Defaults allow 32 event subscribers (8 per actor),
1 MiB per subscriber, 64 outstanding operations (8 per device, 32 per actor),
128 MiB of operation payloads, 32 HTTP requests and 2 large transfers. JSON bodies
are limited to 1 MiB and uploads to 128 MiB, including chunked transfers. Device
history, media queues, authentication sessions and background crash work also
have finite limits. `/v2/health` exposes the configured limits and current usage.
Admission failures return HTTP 429 with `resource_limit` and `Retry-After: 1`;
oversized bodies return 413. Cancelled and completed work releases its capacity.
History responses are capped at 8 MiB; clients continue using the last event ID.
Slow observers receive `history_gap` records when their byte or item budget fills.

Ordinary events default to 7 days, 200,000 rows and a 512 MiB budget. New events
are stored once in SQLite segments, rotating at 8 MiB or one hour, with at most
64 retained segments. Closed segments are retired as whole files, so a row
budget can retain fewer rows and age expiry can lag by one segment's duration.
Segment accounting includes indexes and journal sidecars; maintenance can
temporarily lag a burst by the in-flight batch and its maintenance interval.
Legacy compressed raw logs default to 7 days and 256 MiB. Background maintenance
uses batches of at most 512 rows, with a 50 ms work window each second; a batch
already executing may finish after that window. Bursts can temporarily exceed
the budget. State, credentials, operations, saved artifacts and operation-bound
healthy observations are protected. Slow clients and unavailable event storage
produce explicit `history_gap` records; live delivery continues. A failed
critical state commit rejects a new device write with `storage_unavailable`.
An uncommitted device result remains unknown after restart and is never replayed.

The event writer owns its SQLite connections on a dedicated thread. Its normal
queue holds at most 1,024 events / 4 MiB, with 16 entries / 512 KiB reserved for
critical evidence. Each transaction processes at most 128 queued events or
512 KiB (one oversized event is handled alone). Durable cursors are broadcast
only after commit. A full queue provides live, explicitly unpersisted events
and records a history gap; it does not wait for disk space. Healthy observations
wait for their persistence acknowledgement. Unchanged device snapshots refresh
at most every five seconds. Shutdown drains the writer; an abnormal exit leaves
a durable marker so the next writer reports its uncertain buffer window.

Use `web --retention-policy policy.json` to save overrides for `event_days`,
`event_rows`, `event_bytes`, `raw_days`, `raw_bytes`, `batch_rows` (1–4096),
`segment_bytes` (at least 16 KiB), `segment_seconds` and `segment_files` (1–1024).
Health reports storage failures, lost events and maintenance state. Project
launches rotate process stdout/stderr at 8 MiB with three backups; a direct
launch can use `--process-log PATH` and `--process-log-bytes N`.

For an existing large 0.2 database, preview before maintenance:

```bash
python "$IRIS" storage --state-dir /path/to/state
# Stop this Gateway and back up its state before applying destructive retention.
python "$IRIS" storage --state-dir /path/to/state --apply
# Optional offline physical compaction; allow free disk space for SQLite's copy.
python "$IRIS" storage --state-dir /path/to/state --apply --compact
```

Logical deletion frees pages for reuse. New databases reclaim free pages
incrementally; older 0.2 files may remain physically large until offline
compaction. WAL/SHM sizes are reported separately. No full-table deletion or
VACUUM runs in the startup or device callback. The 0.2 schema upgrade preserves
existing state and adds operation-bound evidence and a segment catalog; it does
not import 0.1 data. Existing events stay queryable in the old database while
bounded maintenance retires them. New IDs continue above the old sequence.
Retiring a segment records its boundary before deleting its files, and an
interrupted event commit becomes a history gap on recovery. Identity bindings
are resolved through the state catalog across closed segments; early bytes
never acquire an invented boot identity. Critical evidence commits before
disposable history, so a missing segment cannot erase an operation's evidence.

### The Workbench says the frontend is not built

Run `npm ci && npm run build` in `tools/frontend`, then restart the Gateway.

### Opening USB resets or disconnects the board

Keep the standard ESP-IDF console enabled. Gateway and flashing/monitoring
tools must not own the same endpoint concurrently. Check DTR/RTS wiring and
the selected reset circuit. Reconnect never deliberately resets the device.

### TCP never becomes reachable

ESP-Iris does not provision Wi-Fi or create a product network interface. Verify
that the application connected, obtained an address, and permits inbound TCP
on the configured port. Use the TCP examples as known-good references.

### Pairing fails

Verify that the device and private Gateway token store contain the same
64-character lowercase hexadecimal token. Never print the token in logs. A
device with an existing NVS token ignores a newly supplied example default
until it is explicitly rotated or reprovisioned.

### Diagnose the local installation

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" doctor --json
```

## Development and tests

From the repository root:

```bash
python3 -m pip install -r components/esp_iris/tools/requirements-dev.txt
cd components/esp_iris/tools
python3 -m ruff check iris_gateway tests
python3 -m pytest

cd frontend
npm ci
npm run test:unit
npm run build
```

Firmware examples are under [`../examples`](../examples/README.md). Build them
from the repository root with paths such as:

```bash
idf.py -C components/esp_iris/examples/minimal -B build-ci-tcp build
idf.py -C components/esp_iris/examples/rpc_jobs -B build-ci build
```

See the [component README](../README.md), [Chinese README](../README_zh.md), and
[wire protocol](../protocol/spec.md) for the device-side integration contract.


## Public host integration

Use `iris_gateway.client` (API major 1) for local project coordination, passive
registry snapshots and terminal operation evidence. Its standard-library-only
API owns state layout and schema checks. Consumer products must not import
`link`, `ownership` or `store` internals. See
[component boundaries](../../../../docs/component-boundaries.md) for compatibility,
source fingerprints, shared lifecycle and product Recovery preconditions.

### Receiver-initiated device handoff

`POST /v2/project/takeovers` runs on the receiving project Gateway. Select exactly
one `device_id` or `endpoint`; optionally pass a UUID `takeover_id`, `force` and
`timeout` (default 120 seconds, maximum 3600). It resolves the current live local
owner and asks that owner to use the existing reserved transfer and validated
acceptance protocol. An unowned device uses `/v2/project/acquire` instead.

Without force, a busy device returns its operation, mirror and Job reasons.
With force, the owner closes admission only for this device, cancels queued work,
lets executing writes finish, stops all mirror channels and waits for cooperative
Job cancellation. Browser log streams and client keepalives do not block handoff.
A drain timeout retains ownership and reopens admission; already stopped work is
not restarted, and a writer is never killed. Transfers use per-device control
locks, so unrelated devices can continue and receivers can accept incoming work
while coordinating an outbound request. Peer failures preserve the takeover ID;
query its durable state before retrying with the same ID.

The public record routes are `GET /v2/project/takeovers/{takeover_id}` and
`POST /v2/project/takeovers/{takeover_id}/{resume|abort|reconcile}`. Responses
contain a `takeover` record with `takeover_id`. Resume runs in the original
receiver, abort in the original owner; completed takeovers cannot be rolled back.
Reconcile requires both original sessions to have exited and a participant
project to prove all physical locks are free.

The receiving request retains its Gateway until validation finishes. It calls
`/v2/project/handoff/prepare` on the owner, which validates the receiver and its
pairing setup before releasing any endpoint. The receiver then validates the
reserved identity locally. This peer endpoint is an implementation detail;
there is no owner-initiated public transfer API, callback accept API or legacy
route alias. The durable registry transaction still uses the internal transfer
schema and retains reservations across interrupted HTTP requests.

### Background evidence exports

`POST /v2/export` now returns HTTP 202 with a job ID, `status_url` and
`download_url`. Poll `GET /v2/exports/{job_id}` (or list `/v2/exports`), then
stream the completed ZIP from its download URL. Workbench Settings provides
range fields, progress, cancellation, download and deletion. Downloads stream
directly to the browser instead of buffering a ZIP in browser memory.

Clients that previously downloaded the POST response as a ZIP must adopt the
job/poll/download sequence. Detect `background-export/v1` in `/v2/health`
when selecting this behavior; deploy Gateway and its bundled Workbench together.
The synchronous offline Python export API remains available as described below.

The optional JSON body accepts `device_id`, `operation_id`, inclusive `since_ns`
and `until_ns`, and `retain` (default false). The project is the current Gateway's
storage root. Operation scope includes its durable evidence plus that device's
logs within the operation interval. Device exports omit project-wide audit
entries that cannot be attributed to the device. A manifest records the effective
scope, event watermark and pruning boundary; `history-gaps.jsonl` and gap events
explain missing history. `events.jsonl` contains resolved device/capture identity
and replaces duplicate raw gzip files and segment databases in the bundle.
Credentials, tokens and the internal state database are never packaged.

One export runs at a time, with default limits of 1 GiB for snapshot plus ZIP and
input bytes, 2 GiB for all exports, 32 retained jobs, 4096 files, and 300 seconds.
`web --export-policy policy.json` saves overrides as `exports.policy` in Gateway
state and `/v2/health` reports the limits and job counts. The policy accepts `job_bytes`, `total_bytes`, `jobs`,
`ttl_seconds` and `timeout_seconds`. Ordinary downloads expire after one hour;
`retain: true` keeps a bundle until explicitly deleted and still counts toward
quotas. `POST /v2/exports/{job_id}/cancel` cancels work, while `DELETE
/v2/exports/{job_id}` removes a terminal job. Failed or cancelled jobs remove
partial data; startup marks interrupted jobs and removes abandoned snapshots.
A disconnected download can be retried before expiry. ZIP and member SHA-256
checksums support validation after download.

Snapshots cover committed history at their recorded watermark; event ingress
continues while an export is accepted. Snapshots briefly seal and hard-link closed segments on the same filesystem,
then back up a SQLite read snapshot and release it before packing. Retention can
continue deleting original segment names while the export reads its bounded
snapshot. The filesystem must support hard links; an unsupported filesystem
produces a failed job with its OS error. Explicit synchronous Python
`store.export_zip(target)` remains available for offline callers, using the same
bounded streaming writer; such caller-owned archives are outside HTTP job expiry.

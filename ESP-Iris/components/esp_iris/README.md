# ESP-Iris

[中文说明](README_zh.md)

ESP-Iris reduces time lost to unnecessary build-and-flash cycles in embedded
development. It makes device logs, state, and operation results easier for
developers and AI agents to understand, control, and trace at the same time.
It turns scattered serial debugging into a unified, structured, and resumable
device-development workflow.

ESP-Iris 0.2 extends the standard ESP-IDF console with commands and bounded
printable API records. `idf.py flash monitor` works without a Gateway. UART
and USB Serial/JTAG use IDF's native reset; S31 application USB uses the bundled
Iris extension to enter ROM and follow USB re-enumeration. One worker serves the control link and an optional,
independently bound data link for media, files and firmware. It does not run
an HTTP server, WebSocket server, JSON parser, framebuffer mirror, or
allocate media-sized buffers.

Multiple devices can connect concurrently to one Python Developer Gateway
through independent USB or TCP endpoints. The Gateway identifies each device
by its stable, eFuse-MAC-derived `device_id`, aggregates and persists fleet events, and fans them
out to the React Web Workbench, command-line clients, and external agents at
the same time.

Use ESP-Iris when a product needs a browser-based engineering interface without
embedding a Web server or debug UI in its firmware.

```text
ESP32 A -- USB CDC0 ---------\
ESP32 B -- USB Serial/JTAG ---+--> Python Gateway + storage --+--> Web Workbench
ESP32 C -- TCP/Wi-Fi --------/                              +--> CLI clients
ESP32 D -- TCP/Ethernet -----/                              +--> External agents
```

Each device has one Gateway owner and independent control/data sessions. The
Gateway and stock monitor take turns owning the console; no device mode switch
is needed. This does not limit the fleet: one Gateway
can supervise many device endpoints concurrently. Workbench, CLI, and agent
connections receive independent event streams; device-changing operations are
serialized per device.

## Chip and transport support

The component declares ESP-IDF >=5.5; this is a dependency floor, not a claim
that every target/IDF/transport combination has been validated. CI currently
uses the pinned ESP-IDF 6.2 development revision in `.github/workflows/ci.yml`.

| Target | Control transport | Fixture / CI group | Validation scope |
| --- | --- | --- | --- |
| ESP32-C2 | UART | `services_c2` / `multichip` | Compile coverage + installed control fixture verified at 74880 baud |
| ESP32-H2 | USB Serial/JTAG | `services_h2` / `multichip` | Compile coverage + installed control fixture verified |
| ESP32 | UART | `services_uart`, explicit target `esp32` / `multichip` | Compile coverage; hardware not validated here |
| ESP32-S31 | UART, USB Serial/JTAG, application USB, TCP | Existing baseline/system groups | Existing S31 validation; no new hardware run in this change |
| Other SDK targets | UART; USB Serial/JTAG where the SoC supports it | Not yet in the build matrix | Unverified |

Run `bash tools/ci/build_firmware.sh multichip` from the repository root in the
pinned IDF environment. It uses separate build directories and explicit targets.
The first three rows compiled locally on 2026-10-16 with IDF revision
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`; that checkout also contained local
camera-driver edits. Remote CI and physical-board validation remain separate.

On 2026-10-16, C2 and H2 hardware checks passed through a consuming workspace's
`mosaico.py`, using this branch's host against the installed `fc63d43-dirty`
service fixtures (no firmware flash). Coverage includes HELLO, status/heap,
binary RPC, control-link screenshot, logs, jobs, 80 sustained RPCs per board,
and software/hardware reset with verified new Boot IDs. The installed C2 image
uses 74880 baud; select it explicitly rather than the usual 115200. Task memory
observation is disabled in both installed fixtures. Files and continuous media
correctly reject the missing data link; OTA and coredump decoding were not tested.
See [the hardware validation record](test_apps/multichip-validation.json) for
firmware identities and evidence hashes. Continuous HELLO probes now discover
reboots on UART/Serial-JTAG even when the port does not re-enumerate; unchanged
HELLO replies do not repeat authentication.

Additional checks on the same date passed with both boards in one Gateway:
180 seconds and 377 RPCs per board, three restarts per board while the peer
remained responsive, recovery after unknown services/methods, invalid arguments
and RPC deadlines, 64-record log bursts, job cancellation, and three ownership
release/reacquire cycles per board. H2 ROM identity probing and application
recovery passed without flash writes. Physical unplug and user-confirmed full cold-power cycles
also passed on C2/H2: automatic reconnection preserved device identity, changed
the Boot ID, and restored binary RPC responses.

The host recognizes image chip IDs through `iris_gateway/chip_targets.py`.
Recognition permits image inspection, but updates still require the device's
explicit target to match the image and preserve any product/board/layout/ABI
contract across Recovery. Unknown IDs and missing targets do not default to S31.
Coredump analysis uses the archived image's chip ID and the matching Xtensa or
RISC-V GDB; `ESP_IRIS_GDB` remains an explicit tool override. UART ROM-entry
reset requires a known live target because its boot timing is chip dependent;
plain run reset and USB Serial/JTAG reset also work without a HELLO identity.

UART and USB Serial/JTAG provide the control link. Files, OTA, system-update
writes and continuous media require a separately connected application USB or
TCP data link as well as the firmware service. Application USB CDC and its ROM
USB download extension remain S31-specific; recognizing another chip does not
open that driver. Additional USB adapters, other IDF versions and board-level
reset circuits need their own validation. Mosaico Recovery ABI/layout policy
continues to belong to the product layer.

## What you get

| Layer | Included capability |
| --- | --- |
| Device component | One bounded worker, binary framing, logs, status, RPC/jobs, media, crash evidence, pairing, and OTA |
| Developer Gateway | Concurrent USB/TCP endpoint supervision, fleet session management, authentication, durable operations, artifacts, and REST/WebSocket APIs |
| Web Workbench | Device overview, logs, RPC, screen/input, media, firmware, operations, records, and settings |
| Command-line client | Scriptable control with stable JSON output for developers and agents |

The default device configuration starts only the core link and log/status
state. RPC tables, media buffers, pairing, and OTA behavior are bounded by
Kconfig and are activated only when configured or used.

## Requirements and compatibility

| Area | Requirement |
| --- | --- |
| ESP-IDF | 5.5 or newer |
| TCP | Console on port 19772 and data on 19773 by default; application owns networking |
| Application USB | ESP32-S31; composite CDC0 console and CDC1 data |
| USB Serial/JTAG | Standard console, reset and JTAG; bounded control services and snapshots |
| UART | Standard text console, RPC and snapshots; preserves the IDF console baud rate |
| Gateway | Python 3.8 or newer; Linux is the primary real-device validation platform |
| Workbench | A current Node.js/npm environment is required to build the bundled React source |

One or more device transports may be compiled into an image. When several are
enabled, physical connections negotiate in bounded turns and the first valid,
authenticated-when-required HELLO_ACK claims the control or data role. Both
roles verify the same device, boot and owner; data can connect first. Losing
one role does not stop the other. A port open by itself never claims the device.

## Quick start

### 1. Add the component

Add ESP-Iris to your application's `main/idf_component.yml`:

```yaml
dependencies:
  lisir233/esp_iris: "^0.2.0"
```

Run `idf.py reconfigure` after adding or changing managed dependencies.

### 2. Select transports

Open `idf.py menuconfig`, then go to:

```text
Component config > ESP-Iris device link > Device transports
```

- **TCP** exposes a printable console and an independent authenticated data port.
- **USB** exposes CDC0 for text/control and CDC1 for binary data. Set
  `CONFIG_TINYUSB_CDC_COUNT=2`; interface descriptors identify their roles.
  `CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD` enables CDC0 download handshakes on S31;
  see [minimal's USB flash setup](examples/minimal/README.md#run-and-verify)
  for loading the host extension and installing the first image.
- **USB Serial/JTAG** and **UART** retain the native console and hardware reset.
  Stop the Gateway before opening stock monitor or flashing on that endpoint.
  RPC, jobs, input and on-demand screenshots work without a data link.
- Applications with an existing ESP-IDF REPL enable
  `CONFIG_ESP_IRIS_CONSOLE_EXTERNAL_INPUT` and register the `iris` namespace via
  [esp_iris_console.h](include/esp_iris_console.h). The product keeps ownership
  of its reader and configures a receive queue for at least two full records.

Select any compatible combination. With multiple transports, provisional
connections have a configurable handshake timeout; `esp_iris_status_t` reports
transport `NONE` while no candidate is negotiating or active.

See the transport-specific notes in the [example index](examples/README.md).

### 3. Start ESP-Iris

```c
#include "esp_iris.h"

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    /* Initialize product services after the early reset-attribution probe. */
    ESP_ERROR_CHECK(esp_iris_start());
}
```

The early, idempotent probe records the running image before later product
initialization can fail. `esp_iris_start()` invokes it as a fallback, but that
cannot cover code which crashes before `esp_iris_start()` is reached. Panic,
watchdog and CPU-lockup resets count toward the default crash-loop threshold;
brownout and power-glitch resets do not unless configured. A normal image that
stays alive for the stable interval, or calls `esp_iris_mark_healthy()`, clears
the count. `esp_iris_start()` is idempotent. `esp_iris_stop()` releases the worker,
transport, VFS, and stdio ownership so the component can be started again.

### 4. Build the firmware

```bash
idf.py build
```

Start with the [minimal example](examples/minimal/README.md) if you want to
validate TCP, USB CDC0, and USB Serial/JTAG profiles before integrating ESP-Iris
into a product.

### 5. Start the Developer Gateway

In a repository checkout:

```bash
ESP_IRIS_COMPONENT_DIR=components/esp_iris
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r "$ESP_IRIS_COMPONENT_DIR/tools/requirements.lock"

cd "$ESP_IRIS_COMPONENT_DIR/tools/frontend"
npm ci
npm run build
cd -

python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web
```

The single lock file uses Python-version markers, so pip selects the validated
dependency set for the active interpreter. Recreate the environment when its
Python major/minor changes. This Gateway runtime is independent of the Python
version required by the selected ESP-IDF release; keep the ESP-IDF tool
environment on the version required by that release.

For a managed installation, set `ESP_IRIS_COMPONENT_DIR` to
`managed_components/lisir233__esp_iris` instead. Open
`http://127.0.0.1:8443/` after the Gateway starts.

Use demo mode to evaluate the Gateway and Workbench without hardware:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --demo
```

See the [Gateway and Workbench guide](tools/README.md) for USB/TCP selection,
authentication, TLS, CLI commands, data retention, and development workflows.

## Capabilities and resource behavior

| Capability | Default behavior | Device-side bound |
| --- | --- | --- |
| Link and status | Enabled | One worker and fixed protocol buffers |
| stdout/stderr logs | Enabled; 8 KiB ring in PSRAM when available | `CONFIG_ESP_IRIS_LOG_RING_BYTES` and `CONFIG_ESP_IRIS_LOG_RING_STORAGE_*` |
| RPC handlers | Registered by the application | `CONFIG_ESP_IRIS_MAX_RPC_HANDLERS` and `CONFIG_ESP_IRIS_RPC_BODY_BYTES` |
| Retained jobs | Created by the application | `CONFIG_ESP_IRIS_MAX_JOBS` |
| Screen/image/audio | Idle until the host starts a stream | One `CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES` buffer per active channel |
| Crash evidence | Read-only when present | Chunked by `CONFIG_ESP_IRIS_CRASH_CHUNK_BYTES` |
| TCP pairing | Disabled by default | One token in `CONFIG_ESP_IRIS_NVS_PARTITION_NAME` and challenge-HMAC state |
| OTA writer | Configurable; cross-project updates allowed by default | Chunked by `CONFIG_ESP_IRIS_OTA_CHUNK_BYTES`; `CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH` opts into matching the running project |
| System inventory | Disabled until a read-only product provider registers | Actual protected-region hashes and last committed operation; no write callbacks |
| System Update | Disabled until recovery registers a product backend | Optional product signature policy, `CONFIG_ESP_IRIS_SYSTEM_UPDATE_MAX_COMPONENTS`, bounded manifest/signature/chunk sizes, and no generic raw-Flash API |
| File service | Disabled until the application registers a logical volume; worker storage is allocated on the first request and released with the session | One file task, one stream, and `CONFIG_ESP_IRIS_FILE_CHUNK_BYTES` per chunk |

The component uses credit-based channels and a latest-chunk policy for media.
A slow host cannot create an unbounded device-side queue.

Normal firmware using PSRAM XIP and external wire buffers can enable
`CONFIG_ESP_IRIS_USB_FIFO_PSRAM` (default on when its prerequisites are enabled).
Only CDC software FIFO bytes move to external BSS: capacities, USB endpoint DMA
buffers, stream metadata and FreeRTOS mutex placement stay unchanged. A checked
build copy adapts TinyUSB 0.21.0~2; managed sources are untouched, and an unknown
CDC source revision fails configuration until reviewed or this option is disabled.
With external BSS enabled, `CONFIG_ESP_IRIS_SERVICE_STATE_PSRAM` also moves the
fixed file-volume registry. Writer firmware keeps its independent internal profile.

`esp_iris_schedule_restart(delay_ms)` reuses the protocol task without allocating
another task or timer. Task/RPC callers own boot-target selection and planned-reset
markers. The restart survives disconnect, waits for the active service callback,
and gives both TX queues a bounded 100 ms flush grace after the requested delay.
Explicit `esp_iris_stop()` cancels it.


The stdout/stderr ring is also mapped into a Core Dump memory section. Sending
a record to the host does not remove its bytes from the retained crash history;
complete records remain until newer records overwrite them. The default storage
is PSRAM when external BSS placement is enabled, with internal DRAM available
for products that prefer stronger crash-time reliability. The Core Dump symbol
`g_iris_log_storage` contains a self-describing header followed by the retained
record ring.

ESP-Iris derives its stable Device ID from the factory eFuse Base MAC. It stores
the TCP pairing token and crash-loop state in the NVS partition selected by
`CONFIG_ESP_IRIS_NVS_PARTITION_NAME` (default: `nvs`). Products may point this
setting at a fixed system metadata partition to isolate retained Iris state from
application NVS.

Register only the directories that the product intentionally exposes, before
calling `esp_iris_start()`:

```c
ESP_ERROR_CHECK(esp_iris_file_volume_register(
    &(esp_iris_file_volume_config_t) {
        .id = "cfg",
        .base_path = "/littlefs/export",
        .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                        ESP_IRIS_FILE_VOLUME_LIST |
                        ESP_IRIS_FILE_VOLUME_MTIME |
                        ESP_IRIS_FILE_VOLUME_WRITE |
                        ESP_IRIS_FILE_VOLUME_DELETE |
                        ESP_IRIS_FILE_VOLUME_MKDIR |
                        ESP_IRIS_FILE_VOLUME_RENAME |
                        ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE,
    }));
ESP_ERROR_CHECK(esp_iris_start());
```

Paths sent over the wire are relative to the registered root. Downloads and
uploads are streamed through the Gateway without buffering a complete file;
downloads support HTTP Range. Uploads use a same-directory temporary file,
strict offset ACKs, SHA-256, `fsync`, and rename. Declare `ATOMIC_REPLACE` only
when the backing VFS provides the required replacement behavior. Rename never
overwrites, delete accepts only files and empty directories, and recursive or
cross-volume operations are not exposed.

## Security model

- USB transports rely on physical access and do not perform link pairing.
- Raw TCP pairing is optional and disabled by default. When enabled, the token
  remains in NVS and the link proves possession using a fresh challenge.
- Loopback Gateway access is authentication-free by default. Non-loopback
  clients require a developer login or named Agent Token. File access by Agent
  Token is separated into `files.read`, `files.write`, and `files.delete`
  scopes; new tokens default to `files.read`.
- Plain HTTP exposes credentials and device data to the local network. Use it
  only on a trusted development LAN, or enable Gateway TLS.
- Wi-Fi credentials, pairing tokens, TLS private keys, and Agent Tokens must
  remain in ignored local configuration or private files.
- System Update is advertised only by a retained recovery image with both a
  read-only inventory provider and a registered write backend. Normal firmware
  may register only the inventory provider for post-reboot verification.
  Signed deployments configure a Gateway trust key and require the backend to
  pin the matching product trust key. Explicitly unsigned deployments omit
  both keys and rely on transport/session access control. In both modes the
  backend must protect fixed system regions and validate every component
  against the manifest plan. Inventory hashes are calculated from the actual
  protected Flash ranges, including erased-byte padding.

## TCP discovery with mDNS

Products that enable the TCP transport can advertise it through DNS-SD after
the product has initialized mDNS and selected its hostname:

```c
ESP_ERROR_CHECK(mdns_init());
ESP_ERROR_CHECK(mdns_hostname_set("my-product-a1b2c3"));
ESP_ERROR_CHECK(esp_iris_mdns_register(NULL));
```

The registration publishes `_esp-iris._tcp.local.` with a unique
`ESP-Iris-<MAC suffix>` instance. ESP-Iris owns only that service; call
`esp_iris_mdns_unregister()` before the product calls `mdns_free()`. Discovery
is local-link metadata, not authentication, and never publishes a pairing
token.

## Examples

All public examples are packaged with the component under [`examples/`](examples/README.md).

| Example | Transport | Start here when you need |
| --- | --- | --- |
| [`minimal`](examples/minimal/README.md) | TCP, USB CDC0 + CDC1, USB Serial/JTAG, or all three | Identity, lifecycle, status, logs, and transport arbitration |
| [`tcp_wifi`](examples/tcp_wifi/README.md) | TCP | Application-owned Wi-Fi and DHCP |
| [`tcp_pairing`](examples/tcp_pairing/README.md) | TCP | Challenge-HMAC pairing and token provisioning |
| [`rpc_jobs`](examples/rpc_jobs/README.md) | USB CDC0 + CDC1 | RPC handlers and cancellable jobs |
| [`display_input`](examples/display_input/README.md) | USB CDC0 + CDC1 | Screenshot, screen mirror, and pointer input |
| [`media_streams`](examples/media_streams/README.md) | USB CDC0 + CDC1 | Synthetic image and PCM audio streams |
| [`file_transfer`](examples/file_transfer/README.md) | USB CDC0 + CDC1 | Streamed file upload/download and metadata mutations |
| [`ota`](examples/ota/README.md) | USB CDC0 + CDC1 | Recovery-first/direct OTA, acceptance, and rollback |
| [`file_service`](examples/file_service/README.md) | USB CDC0 + CDC1 | FATFS logical volume and bounded file operations |
| [`crash_recovery`](examples/crash_recovery/README.md) | USB CDC0 + CDC1 | Retained Core Dump and factory recovery after repeated crashes |
| [`lifecycle`](examples/lifecycle/README.md) | USB CDC0 + CDC1 | Stop, unregister, restart, and reconnect |

Hardware-focused internal fixtures remain under `test_apps/`; they are not part
of the published component archive.

## Documentation map

- [Public C API](include/esp_iris.h)
- [System Inventory provider API](include/esp_iris_system_inventory.h)
- [System Update backend API](include/esp_iris_system_update.h)
- [Protocol constants](include/esp_iris_protocol.h)
- [Wire protocol 0.2](protocol/spec.md)
- [0.1 to 0.2 migration](protocol/migration-0.2.md)
- [Golden protocol vectors](protocol/golden_vectors.json)
- [Gateway and Workbench](tools/README.md)
- [Examples](examples/README.md)
- [Engineering architecture](https://github.com/esp-mosaico/esp-mosaico-utils/blob/main/ESP-Iris/docs/esp-iris-architecture.md)
- [Changelog](CHANGELOG.md)

## Version and license

This source tree targets `0.2.0`; use this checkout until that release is published.
Device protocol and host API 0.2 require matching firmware and Gateway versions.
ESP-Iris is licensed under [Apache-2.0](LICENSE).

### Deferred file-volume mounting

Register always-mounted filesystems with `esp_iris_file_volume_register()`.
For slow or optional media, use `esp_iris_file_volume_register_deferred(config,
status, user_ctx)` before starting Iris. It publishes the volume descriptor
without filesystem I/O. The nonblocking, thread-safe status callback returns
`ESP_ERR_INVALID_STATE` while mounting, `ESP_OK` once the root directory is
ready, or a media error after failure. File path requests map these to the
existing BUSY/OK/IO statuses; OTA and other services remain available. Keep
mounted storage and callback context alive until Iris stops and closes its
handles. Applications own mount scheduling and retries; Iris never mounts or
formats media in a request handler.

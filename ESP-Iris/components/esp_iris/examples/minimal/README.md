# Minimal link, status, and log example

This is the recommended first ESP-Iris firmware project. It starts the device
component, prints the persistent device ID, and emits a status line every five
seconds with transport, lifecycle, link/session state, uptime, frame counters,
dropped log bytes, and minimum remaining worker stack.

The example registers no RPC or screen backend and has no eligible OTA slot,
so it also provides a clean baseline for resource and unsupported-operation
behavior.

## Transport profiles

| Profile | Defaults file | Notes |
| --- | --- | --- |
| TCP | `sdkconfig.defaults` | Independent text/control and binary data ports; an application layer must provide networking |
| Application USB CDC0 + CDC1 | `sdkconfig.usb.defaults` | ESP32-S31; CDC0 text/control, CDC1 binary data |
| USB Serial/JTAG | `sdkconfig.usj.defaults` | Standard text console with Iris commands; JTAG remains available, no data link |
| Multi transport | `sdkconfig.multi.defaults` | ESP32-S31; control and data links negotiate independently and bind to the same owner/device/boot |

## Build

From the repository root, build each profile in an isolated directory:

```bash
idf.py -C components/esp_iris/examples/minimal \
  -B build-minimal-tcp \
  -D SDKCONFIG="$PWD/build-minimal-tcp/sdkconfig" build

idf.py -C components/esp_iris/examples/minimal \
  -B build-minimal-usb \
  -D SDKCONFIG="$PWD/build-minimal-usb/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.usb.defaults build

idf.py -C components/esp_iris/examples/minimal \
  -B build-minimal-usj \
  -D SDKCONFIG="$PWD/build-minimal-usj/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.usj.defaults build

idf.py -C components/esp_iris/examples/minimal \
  -B build-minimal-multi \
  -D SDKCONFIG="$PWD/build-minimal-multi/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.multi.defaults build
```

The explicit per-build `SDKCONFIG` paths are required when profiles are built
one after another. A generated project-level `sdkconfig` otherwise takes
precedence over the next profile's defaults.

## Run and verify

Flash through a programming port appropriate for the board. For application
USB CDC0, the first image normally uses UART/Serial-JTAG or a manually entered
ROM downloader. Stop the Gateway before flashing through a USB Serial/JTAG
endpoint used by ESP-Iris.

The S31 application USB profile enables `CONFIG_ESP_IRIS_USB_AUTO_DOWNLOAD`.
After installing it once, `idf.py -p CONSOLE_PORT flash monitor` can use the
same high-speed USB connector: CDC0 recognizes the BSP-style DTR/RTS download
handshake, while CDC1 remains the data interface. The project `idf_ext.py`
loads Iris's host extension before the first build. It builds before reset,
follows the selected USB location into ROM, checks the MAC in its USB serial descriptor, calls
IDF's original flash target, and waits for the matching application console
before starting monitor with `--no-reset`. Multiple boards require explicit
`-p` selection. Release both Gateway connections and any other monitor first.
The extension reserves both CDC interfaces until monitor exits.
Identity checks do not open the ROM CDC: closing a separate `esptool read-mac`
session can reset S31 out of ROM even with `--after=no-reset`.
Exit monitor before starting another flash; its built-in reflash shortcut does
not yet hand off the parent process's USB locks to its child.

For other projects, load the component's `idf_ext.py` through IDF's trusted
component extension mechanism, or explicitly import `action_extensions` from
`tools/iris_usb_flash` in a project `idf_ext.py` as this example does. Reconfigure
existing build directories after enabling the option. This handshake requires
the new firmware on the device; old firmware still needs manual Boot/ROM entry
for its first installation. The automatic path requires USB physical-location
and interface metadata (validated with Linux-style enumeration).

After the Gateway connects, logs should contain the device ID and periodic
status lines. A local `esp_iris_stop()`/`esp_iris_start()` cycle keeps the same
`boot_id`; a real reboot produces a new one.

In the multi profile, a socket or serial descriptor is only a discovery hint.
Each link must complete its role-specific handshake within the claim timeout.
Control and data choose their transport independently and verify a shared
owner, device identity and boot identity. A data link may connect first; losing
candidates for the same role yield without disconnecting the other role.
An unresponsive candidate cannot retain ownership indefinitely.

The 2 MB defaults intentionally contain no coredump partition. Crash metadata
remains queryable, while `core_dump_present` is false. Use the internal
`test_apps/coredump` fixture when validating Flash coredump transfer.

## Next steps

- Add networking with [`tcp_wifi`](../tcp_wifi/README.md).
- Add authenticated TCP with [`tcp_pairing`](../tcp_pairing/README.md).
- Add RPC/jobs with [`rpc_jobs`](../rpc_jobs/README.md).
- Return to the [example index](../README.md).

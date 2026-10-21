# ESP-Mosaico Vibe Mode

The [0.2 software specification (中文)](docs/esp-mosaico-vibe-mode-software-spec-zh.md)
defines the architecture. The reviewed base bundle is now 0.2.1, with Recovery
ABI 2 and QIO. The [Linux nine-device validation record](docs/vibe-0.2.1-nine-device-validation-20261021.json)
records 162/162 parallel updates across three rounds, release image hashes,
timings and remaining validation limits. Earlier architecture checks remain in
the [0.2 validation record (中文)](docs/validation-0.2.md).

Vibe Mode is the ESP-Mosaico application installation and device maintenance mode.
It runs ESP-Iris; ROM Download Mode is the chip flashing path without ESP-Iris.
The product directory is `esp-mosaico-vibe-mode`; its boot policy
uses Recovery ABI 2 and the `mosaico-retained-test-2m-v2` layout.

This component owns retained Vibe Mode firmware, its reviewed bundle and the
public product ABI in `include/mosaico_recovery_contract.h`. The header is used
by independently built normal applications and Vibe Mode. The 64-byte sysmeta
record now uses version 2; one-shot boot intent uses `mosaico_boot_v2`.
The 0.2 ABI does not accept 0.1 firmware or update bundles.

The product CLI and build runner now live in [mosaico-tools](../mosaico-tools/README.md).
The old local launcher, Python forwarding package and build-runner entrypoint
have been removed. Consumers use `mosaico-tools`.
Integration and firmware tests remain under `tests/`.
See the [nine-device flashing reproduction plan (中文)](docs/multi-device-flash-reproduction-plan.zh-CN.md)
for large-image, layout-switching and repeated-update hardware experiments.
The [source-built self-update issue record (中文)](docs/multi-device-self-update-issues.zh-CN.md)
preserves the first concurrent failure and device-side observations.

To add Vibe Mode and Iris to an existing application, follow the
[0.2 migration and boot contract (中文)](docs/migration-0.2.md).

The firmware project is `firmware/vibe-mode`, with CMake project name `vibe-mode`
and build artifacts `vibe-mode.bin` and `vibe-mode.elf`. The CLI, application
templates and CI all resolve this location. Reconfigure in a fresh build
directory after migrating; old CMake caches contain the previous absolute paths.
The `recover` launcher uses `build-mosaico-vibe-mode` for this purpose.

The reviewed bundle remains under `firmware/vibe-mode/prebuilt/recovery` with
its original `factory` project identity, `factory.bin` and manifest hashes.
Current-source bundles use `vibe-mode.bin`; both retain the Recovery profile,
ABI and partition layout. Replace reviewed images only after full validation.
Perform device operations through the consuming workspace's `mosaico.py` launcher.

<a id="recovery-through-an-independent-usb-serialjtag-connection"></a>

## Restore base firmware through an independent USB Serial/JTAG connection

When the same board has a separate USB Serial/JTAG cable connected, select its
port explicitly while the primary ESP-Iris connection is still live:

```sh
python mosaico.py recover --device-id DEVICE_ID --recovery-port COM14 --source current
```

Omit `--source current` to use the reviewed bundle. `--dry-run` resolves the live
identity and port without building, resetting or writing. This option
requires exactly one connected Espressif `303A:1001` interface, and the named
port must identify it. Selecting it asserts that this independently connected
interface belongs to the chosen board. For a device already in ROM download
mode, prefer `--hardware-mac`; its eFuse identity is read directly instead of
inferring an association from USB topology.

The command prepares the complete reviewed/current Vibe Mode bundle before
submitting one local Gateway operation. The operation preserves crash evidence
and detaches both the managed interface and the explicit programming interface.
A separate foreground executor holds physical locks on both endpoints while
writing. The serial interface is enumerated again before the write; an identity
change, ambiguous endpoint or conflicting owner prevents flashing. Other
applications must release the serial port before this operation can use it.

Only the existing `mosaico-recover-flash` target writes firmware. Its complete
bundle writes bootloader, partition table, OTA selection data and Vibe Mode
firmware in `vibe_mode` (`app/test`); this option does **not** introduce whole-flash
erase. The installer otadata contains a one-shot Vibe Mode bootstrap marker.
Native normal-application flash uses standard blank otadata and preserves the
maintenance image. Preserve the complete bundle/layout contract.
Vibe Mode acceptance uses the original managed connection: the same Device ID,
a new Boot ID, the prepared Vibe Mode version and OTA capability must be verified.
The operation record contains before/after evidence and links to the raw writer
log. Its operation ID is included in the command result and `host-operation.json`.
There are no separate endpoint leases, tokens or manual abort/renewal steps.
Failure remains recorded after the temporary process-owned exclusion ends.

Vibe Mode remote downloads now use [HTTPS Bridge](firmware/vibe-mode/README.md).
Configure the build Origin and board ID, then use `mosaico.py iris test bridge-code` or
the device download page to pair once for a partitions, layout or factory update.

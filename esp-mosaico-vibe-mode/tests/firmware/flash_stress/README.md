# Flash stress fixture

Build `A`, `A2`, `B` or `C` with the CMake cache setting `STRESS_SAMPLE`.
Each sample is built from source; generated inputs stay in the build directory.
The normal application uses Recovery ABI 2 and the retained Vibe Mode writer.
This hardware workload has no UI or Wi-Fi dependency.

`A` and `A2` have distinct embedded 4 KiB payloads. `B` embeds a deterministic
11.25 MiB payload in a valid application image. `C` changes the application
boundary and installs two independently verified 1 MiB resource partitions.
The resource bytes come from the baseline application region. Both layouts
preserve the system prefix, NVS and the existing `ui_apps` region.

Startup computes the embedded payload and complete resource SHA-256 hashes
before the product adapter may report HEALTHY. RPC service `0x6a03`, method `1`,
with an empty request returns JSON containing sample identity, payload size and
digests, including the unchanged `ui_apps` digest. The RPC does not write data.
Keep one host-side `ui_apps` baseline per device and compare it after every step.

Use the formal `prepare_system_update.py` and Iris bundle builder for frozen
application/layout/resource bundles. Omit NVS, `ui_apps`, bootloader and Vibe Mode
images. Inspect the complete write plan before installing. Every device operation
must use a consuming workspace's `mosaico.py`; direct flashing is not permitted.

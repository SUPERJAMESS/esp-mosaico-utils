# Iris internal RAM acceptance fixture

Test-only firmware using the real generated application selected by `MOSAICO_TEST_APPLICATION` and normal
partition/recovery contract. Normal applications do not compile `budget.c`.

Build with the workspace low-noise IDF build runner and a compatible ESP32-S31
ESP-IDF environment, then install only through the product workflow:

```sh
python mosaico.py iris system-update --project submodule/esp-mosaico-utils/esp-mosaico-vibe-mode/tests/firmware/iris_internal_budget --skip-build
```

Discover the live Device ID with `python mosaico.py iris list`. Run `run_test.py`
using the prepared ESP-Iris host Python (with aiohttp), after retaining the selected project with `python mosaico.py iris run --project projects/my_app`.
Pass its printed Gateway URL explicitly:

```sh
python submodule/esp-mosaico-utils/esp-mosaico-vibe-mode/tests/firmware/iris_internal_budget/run_test.py --workspace . --gateway http://127.0.0.1:PORT --application-name my_app --device-id DEVICE_ID
```

The script uses only the existing managed Gateway HTTP/WebSocket API. It tests
1024-byte echo RPCs, built-in recovery-state/system-inventory RPCs, standalone screenshots, concurrent RPC/screenshot during
mirroring, complete 480x480 RGB565 frames, unchanged Boot ID, and error counters.
JSON results, operation IDs, PNGs and raw responses are retained under
`.codex-runs/mosaico/*-iris-budget-test/`. A failed request is not a passing run.
Restore the original application and its layout afterwards with `mosaico.py iris
system-update`. `app-update` is valid only when the complete partition tables
are identical. Archive the matching BIN, ELF and map before installing a bundle
from a non-default build directory.

Metrics also report the named TinyUSB task's minimum free stack; acceptance
requires at least 512 B. A test-only timer allocated before attribution emits
`iris_budget: EXIT current=... peak=... errors=...` during the delayed transition to Vibe Mode, before reset. The 0.2 fixture wraps
`mosaico_boot_request_recovery()` to arm this timer. Use managed `mosaico.py iris logs` to preserve
that record during the final normal -> Recovery -> normal installation.

After restoring production firmware, `--production-check --rpc-count 10
--screenshots 1 --seconds 5` checks its real state RPC/screenshot/mirror behavior
without calling diagnostic RPCs. Add `--firmware-mode recovery
--application-name factory` to exercise Vibe Mode. Such a report reads
whole-device `/v2/devices/{id}/memory` statistics, explicitly sets
`budget_measured=false`, and does not attribute that memory to Iris.

`--snapshot-path control` forces standalone full-size screenshots through the
printable control link. While mirroring, that explicit request must return the
documented busy error; a data-path screenshot reuses the active mirror frame.
After stopping the mirror, the selected standalone path must work again.
Pass repeated `--control-device DEVICE_ID` arguments to add H2 USB Serial/JTAG
and C2 UART services fixtures on the same Gateway during the entire mirror phase.
Each performs a control screenshot, continuous tagged 256-byte echo RPCs and
filtered event/log Follow. Results retain per-device latencies and reject identity,
Boot ID, crash/invalid-counter changes or cross-device event delivery.

Initial/final Boot IDs and error counters are recorded: acceptance rejects new
invalid frames or crashes during the workload, without treating historical
counter values as newly introduced errors. Instrumented acceptance retains the
strict 25,000-byte internal peak limit and 512-byte TinyUSB stack margin.

Internal peak = charged internal heap high-water mark + admitted static DRAM +
resident IRAM. Allocation hooks charge actual internal addresses, allocator
rounding and a conservative 16 B/block overhead. They attribute Iris, service,
TinyUSB and tcpip tasks, bootstrap/recovery registration scopes, plus all ISR
allocations. Stacks and task control blocks are counted at creation, including
the normal application's recovery health task. Free hooks run regardless of
which task frees a tracked block. Trace errors invalidate a result.

`map_report.py` charges the complete Iris/TinyUSB/USB HAL/PHY linked input
sections, linked network state, recovery adapter and screen backend. Shared
OS/NVS implementation statics and the test-only hook/table are excluded;
bootstrap NVS heap allocations are included. Diagnostic RPC registration and
capability differences conservatively enlarge the fixture's footprint versus
normal GSP. This is a bounded workload measurement, not a bound on arbitrary
future RPC callbacks. Actual network connections and Recovery's writer runtime
are outside this acceptance scope.

Static input sections are rounded up to their product 4 B alignment. Historical pressure JSON captured before the conservative ledger update remains
unchanged; current results include alignment directly. External FIFO/registry BSS
is excluded only by its actual ELF address/section. Inspect the ELF to confirm
that CDC stream/mutex and endpoint DMA objects still reside in internal RAM.

PSRAM buffers use explicit SPIRAM capabilities without internal fallback.
Normal PSRAM log storage can lose crash-adjacent logs while flash cache is
disabled; Recovery retains its independent internal log profile.

Configure with `-DMOSAICO_TEST_APPLICATION=/absolute/path/to/generated/app` and
`-DMOSAICO_BSP_ROOT=/absolute/path/to/bsp`; instrumentation is injected only by
this test project after IDF registers the application main component.

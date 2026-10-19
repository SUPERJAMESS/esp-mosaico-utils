# Resource gate baseline (IRIS-T02)

The previous budget file used paths from another repository and was not run.
Several files already exceeded its limits at review baseline `b484e47`. This
change explicitly adopts the measured integration size, rounded upward to the
next 50 lines, while preserving existing limits where they remain sufficient.
The current review includes hardware-backed identity and lazy service lifecycle
changes.
It does not claim that larger modules are desirable or that line counts prove
latency, memory use, or maintainability. Further growth must change this reviewed
file; the checker fails on missing files, wrong path types and empty groups.

The executor is separately budgeted because it owns asynchronous service work.
Compatibility, reconciliation and request identity have separate host modules.
Workspace.tsx and the existing Gateway remain refactoring debt; their pre-existing
growth is not hidden as a performance improvement in this patch.

Task memory observation later added status fields and a control response to
`esp_iris.c`. Its 1255-line measurement raises that file's reviewed ceiling to
1300. Gateway snapshot assembly now lives in `memory_observation.py`, keeping
`gateway.py` within its existing 2050-line limit.

| Source | Review baseline lines | Integration lines | Previous limit | New limit |
| --- | ---: | ---: | ---: | ---: |
| `components/esp_iris/src/esp_iris.c` | 998 | 1255 | 1100 | 1300 |
| `components/esp_iris/src/esp_iris_services.c` | 1946 | 2002 | 1900 | 2050 |
| `components/esp_iris/src/esp_iris_files.c` | 1685 | 1718 | 1700 | 1750 |
| `components/esp_iris/tools/iris_gateway/gateway.py` | 1960 | 2049 | 1250 | 2050 |
| `components/esp_iris/tools/iris_gateway/session.py` | 1108 | 1259 | 1200 | 1300 |
| `components/esp_iris/tools/iris_gateway/files.py` | 509 | 509 | 550 | 550 |
| `components/esp_iris/tools/iris_gateway/file_routes.py` | 353 | 368 | 400 | 400 |
| `components/esp_iris/tools/frontend/src/Files.tsx` | 237 | 237 | 300 | 300 |
| `components/esp_iris/tools/frontend/src/Workspace.tsx` | 1525 | 1525 | 550 | 1550 |
| `components/esp_iris/src/esp_iris_executor.inc` | new | 414 | 400 | 450 |
| `components/esp_iris/tools/iris_gateway/compatibility.py` | new | 67 | — | 100 |
| `components/esp_iris/tools/iris_gateway/reconciliation.py` | new | 130 | — | 150 |
| `components/esp_iris/tools/iris_gateway/operation_identity.py` | new | 42 | — | 50 |

Firmware byte gates use the CI build directories. The existing services/coredump
limits are retained. The new inventory/update test binaries are limited to 1 MiB;
their 2 MiB flash profiles still rely on ESP-IDF's partition fit check. The
frontend remains limited to 400 KiB. Missing build artifacts fail the selected
gate; local source-only checks deliberately do not claim a firmware byte result.

The USB services fixture uses the compiler's size optimization (`-Os`) with the
same service configuration, buffers and task stacks. On the pinned ESP-IDF
revision, the 0.2 image is 414,192 bytes, below its unchanged 471,859-byte limit;
the previous performance-optimized image was 496,048 bytes. This is a build-size
result for the fixture, not a new hardware throughput or peak-memory measurement.

CI runs the host matrix and separate C ASan/UBSan job. macOS uses the explicit
Intel runner label published in [GitHub's runner documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-where-workflows-run/choose-the-runner-for-a-job),
so Python 3.8 does not depend on an ARM-only interpreter build. Configuring that
job is not evidence it has run: local acceptance reports list actual platforms.

Release indexing requires the declared application BIN/ELF/map, sdkconfig,
bootloader and partition table, and checks the IDF path against the build
description. It records hashes of tracked and untracked, non-ignored source
files as well as revision/dirty status. Indexing existing artifacts is not a
substitute for a successful build of that source snapshot or for hardware tests.

The bootloader-logo CI repair separates OTA request handling from the service
registry, executor dispatch/response delivery from worker lifecycle, and the
Gateway device-info model from session transport. Existing ceilings stay in
place; the extracted modules have explicit 400, 200 and 100 line ceilings,
respectively. C include fragments preserve the existing translation unit and
private state, and `session.DeviceInfo` remains available to existing callers.

The file service's path validation, request decoding and filesystem metadata
helpers now live in the private `esp_iris_file_paths.inc` fragment (366 lines,
400-line ceiling). This reduces `esp_iris_files.c` from 1760 to 1398 lines while
preserving its existing 1750-line ceiling. The fragment keeps the same static
functions, volume state owner and translation unit; file operations and wire
behavior are unchanged.

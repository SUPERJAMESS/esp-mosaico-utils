# Application integration

Vibe Mode is the retained firmware that runs ESP-Iris; ROM Download Mode is the
chip flashing path without Iris. Recovery component paths, ABI and protocol
names retain their technical spelling. The 0.2 implementation uses Recovery
ABI 2 and the `mosaico-retained-test-2m-v2` layout. See the
[0.2 migration guide](migration-0.2.md).

For Vibe Mode installation and optional Iris logging in an existing application,
start with the [migration guide (中文)](../../docs/recovery-iris-migration.zh-CN.md).
The integration below adds the complete Mosaico application update workflow.

The tools repository owns `templates/hello_world/mosaico-template.json` and the
public `mosaico.py project init` / `project sim` commands. Creation needs Python
3.8+, Git 2.28+ and this utilities checkout, with no IDF, BSP or device requirement.

Template schema v1 provides `workspace`, `template`, `utils`, `tools`, `bsp`,
`esp_iris`, and `engine` path anchors. Paths are rendered relative to each
output file. `workspace.init_template` remains configurable. `game create/new`
defaults to the tools-owned `templates/blank_game/mosaico-template.json`.
`--template shooter`, `sky-hop` or `tower-defense` uses the Engine game creator
to copy a maintained game with its private shared launcher/Board. The blank
template uses the exclusive application writer; Engine copies also refuse to
overwrite an existing project. Configure `dependencies.raylib`
for the engine location; it is only required for games.

All creators supplement missing files from the tools-owned project management
profile (`templates/common/git_quality/mosaico-template.json`) and initialize a
separate Git repository on `main`. This profile contains configuration and guides;
its hooks reference `tools/quality/check_project.py` in the same utils checkout
used by the workspace. The checker is maintained and tested centrally, and its
version follows the workspace's pinned utils revision. It is not copied into apps. Existing template
configuration is preserved and reported; paths that conflict as files/directories
are errors. Project creation does not install hooks or make a first commit.
The generated quality guide documents enabling pre-commit, its eight categories
of checks, and manual Cppcheck analysis using the actual compilation database.
Custom templates retain schema v1 and their application layout. The public JSON
result adds `git` (status/branch) and `quality` (guide, preserved files and setup
commands); `files` includes management files but excludes internal Git metadata.
Dry-run reports the planned setup without writing. An Engine dry-run uses the
Engine's reported file inventory to identify preserved configuration; if the
Engine omits that inventory, preservation is determined during actual creation.

The blank game provides shared C state/update/rendering with separate device and
Host adapters, generated project identity and the retained Vibe Mode layout. It
uses the Engine shared native launcher and selected Board for display, input,
hardware access. The Iris wrapper explicitly selects `esp_mosaico_raylib_iris`
for USB startup, screenshot/pointer registration and first-frame health acceptance. It contains no game assets,
GSP canvas placeholder or external resource partition. Host and native use the
same `raylib_lite_game_module_v1()` implementation.

Normal apps include `esp-mosaico-recovery/cmake/mosaico_idf_project.cmake`
before project(), supplying MOSAICO_BSP_ROOT for the board-owned splash handoff.
Add `esp-mosaico-recovery/components/esp_mosaico_app_recovery` to
EXTRA_COMPONENT_DIRS and call iris_ota_support_start(). This component has no
GSP, BSP or display dependency. Its configure gate validates the effective
configuration, including existing sdkconfig files; the OTA writer stays in Vibe Mode.
For the S31 retained product, keep `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` in both
applications and Vibe Mode. Native application flashing also replaces the
bootloader, so all retained product images must agree on the Flash bus mode.
The shared defaults and both build/host checks
enforce this setting, including reused builds. This product constraint does
not apply to independent third-party IDF examples.

The pinned SDK intentionally emits `--flash-mode dio` for the bootloader image
even when QIO is selected: its bootloader enables quad mode during initialization.
Check `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` and `CONFIG_ESPTOOLPY_FLASHMODE_VAL=1`
in the effective configuration; do not override the generated flashing arguments.

Optional GSP components are `mosaico-tools/components/esp_mosaico_gsp_bundle`
(ui_bundle_open) and `esp_mosaico_gsp_iris` (iris_screen_mirror_init/attach).
The latter owns the display-presenter link wrapper. Application display and touch
policy stays in the template's board_display.c. Both components retain ESP-GSP
1.5.1 and the MOSGSP resource format.

Use `cmake/gsp_compiler.cmake` before IDF to resolve the pinned GSPC. Include
`cmake/gsp_bundle.cmake` in the application component, then call
`mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")`.
Applications use GSPC 0.6.1 with ESP-GSP 1.5.1. Vibe Mode independently retains
GSPC 0.5.0 with ESP-GSP 1.4.0, selecting `MOSAICO_GSPC_VERSION` before including
the shared compiler bootstrap. Explicit `GSPC_EXECUTABLE` overrides must match
the project's component version.
Include `cmake/system_update.cmake` after project() to declare System Update
artifacts and verify the native `flash`/`app-flash` layout. Other resources
use MOSAICO_SYSTEM_UPDATE_DATA_LABELS and per-label IMAGE/TARGET global properties.
No normal application bundle replaces the retained bootloader.

With a matching 0.2 base installation, stock `idf.py flash monitor` writes the
normal image to `main_app` (`app/ota_0`, `0x210000`). The generated native flash
manifest also contains the matching product bootloader, partition table and
standard blank otadata; it does not write `vibe_mode` (`app/test`, `0x20000`).
The shared CMake entry point installs the product bootloader selection code.
Check the complete `flasher_args.json`, including declared resources, before
device validation. Use `mosaico.py recover` first on a blank or incompatible board.

The default delivery flow remains `mosaico.py iris system-update` or, for an
identical full partition table and code-only changes, `iris app-update`.
Firmware and file delivery require the independent Iris data link. UART and
USB Serial/JTAG provide control, logs, RPC and on-demand screenshots; they do
not carry Iris firmware updates. Stock IDF ROM flashing is independent of this
restriction. Close the owning Gateway before opening stock monitor on the same
console. No Iris host extension or device protocol-mode switch is needed.
With HS USB, the ROM and application CDC interfaces re-enumerate on reboot.
The pinned IDF Monitor 1.9.0 exits if its first port-open occurs during that
gap; start stock `idf.py monitor` after the application port appears. UART and
USB Serial/JTAG keep their native combined flash/monitor workflow.

System Update bundles use `esp-iris-system-update/0.2`. The old `/v1` and `/v2`
manifest names are rejected. Rebuild old applications and bundles with the
matching tools instead of relabeling their manifests.

Vibe Mode's `product_contract.json` owns host identity and fixed partition values.
The C ABI remains in `esp-mosaico-recovery/include/mosaico_recovery_contract.h`;
contract tests compare the product manifest, configuration and partition tables.

Host validation: run `pytest mosaico-tools/tests` and
`pytest esp-mosaico-recovery/tests/test_*.py` separately. Recovery UI tests also
require its pinned GSPC 0.5.0 and simulator 1.4.0. Hardware acceptance remains
separate from these host checks.
No consumer workspace source is needed. Old workspace paths have no forwarding
layer; consumers migrate to these public entry points and rebuild.

## User-owned Raylib Lite games

Develop a game in its own repository, then build an Iris installation bundle:

```sh
python mosaico.py game build /path/to/my_game --target iris
# A relative path or --project /path/to/my_game works too.
python mosaico.py game build sky_hop --target iris
```

Configure `dependencies.raylib` with a current Engine source checkout. Named
examples and explicit project paths use the same wrapper. Explicit projects
need a top-level ESP-IDF `CMakeLists.txt` containing `project(...)` and a
`main/CMakeLists.txt` component. They do not need registration in Engine examples.
Use the Engine native game lifecycle and shared launcher, as demonstrated by its
games. This wrapper supplies `app_main` through that launcher. Applications with
their own `app_main` use the ordinary native build and application integration
path described above.

The wrapper references the original `main/` and optional `components/` directory,
so component-relative resources and `idf_component.yml` dependencies stay with
user source. Source files, manifests and existing build/sdkconfig files are not
copied or edited. Generated projects and build output live under the configured
workspace run directory, with separate directories for equal game names at
different paths. Native and Iris builds therefore do not share sdkconfig.

The wrapper supplies the Engine Board and common native launcher, then invokes
existing Recovery-first packaging. It consumes user `sdkconfig.defaults` and
`sdkconfig.application.defaults`; Iris-specific defaults enable required application services
and select USB management. The product partition table comes from the shared
`blank_game` template. A project's own partition CSV and top-level CMake build
logic are not used by this wrapper; declare game resources and custom component
logic in component CMake files. Use ordinary native builds for other layouts.

An optional `version.txt` supplies the numeric CMake project/bundle version
(default `1.0.0`). The generated app version default matches it; a game's explicit
`CONFIG_APP_PROJECT_VER` still takes precedence. Installation is a separate
operation through Vibe Mode; the build command does not flash a device.

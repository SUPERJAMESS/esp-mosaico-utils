# ESP-Mosaico Utils agent rules

User-facing ESP-Mosaico firmware is **Vibe Mode**; **ROM Download Mode** is the
chip download program without ESP-Iris. Vibe Mode sources live in
`esp-mosaico-vibe-mode/firmware/vibe-mode` and the firmware project is `vibe-mode`.
Keep Recovery commands, protocol identifiers and ABI names stable. See the
[firmware guide](esp-mosaico-vibe-mode/firmware/vibe-mode/README.md).

## Repository boundaries

- `ESP-Iris/` owns the reusable ESP-IDF component, protocol, Developer
  Gateway, Workbench, examples, and component-level tests.
- `mosaico-tools/` owns the ESP-Mosaico host CLI and build runner. It consumes
  the public Iris host API and product Recovery contracts.
- `shared/` owns GSP tool acquisition and Ideas display-resource generation.
  Consumers supply versions and paths; shared modules must not import consumer
  implementations or locate their internal files. Run `shared` tests for changes.
- `esp-mosaico-vibe-mode/` owns retained Vibe Mode firmware, its public product
  ABI header, reviewed bundle, and integration tests. Legacy tool paths are removed.
- Do not recreate a nested ESP-Iris submodule. Both products resolve from the
  same `esp-mosaico-utils` revision.
- Keep consuming applications and workspace-specific `.mosaico.json` files in
  their application repositories. Reusable templates live in mosaico-tools/templates;
  optional GSP application components in mosaico-tools/components; the normal
  Recovery adapter and its CMake configuration gate live in esp-mosaico-vibe-mode.
- Run both mosaico-tools/tests and esp-mosaico-vibe-mode/tests for public application
  integration changes. These tests must not read consuming workspace internals.

## Versions

- ESP-Iris and ESP-Mosaico Tools are pre-1.0 products on the `0.2.x` release
  line. Their release versions are independent even when released together.
- ESP-Iris has its version source in
  `ESP-Iris/components/esp_iris/idf_component.yml`.
- ESP-Mosaico Tools has its version source in
  `mosaico-tools/tools/mosaico_cli/__init__.py`.
- The retained Vibe Mode firmware version is a separate on-device compatibility
  identifier. Do not reset or downgrade it merely to match the tools release.
  The 0.2 architecture uses Recovery ABI 2 and layout version 5. Keep 0.1
  migration guidance in documentation; do not add protocol or code compatibility.
  Publish the 0.2 prebuilt bundle only after the validation below.
- Use product-specific tags: `esp-iris-vX.Y.Z` and
  `esp-mosaico-tools-vX.Y.Z`.

## Validation

- Run Vibe Mode host tests from `esp-mosaico-vibe-mode/tests`.
- Run ESP-Iris Python tests from `ESP-Iris/components/esp_iris/tools` and keep
  Python 3.8 compatibility.
- Run Workbench unit/build/E2E checks for frontend changes.
- Build affected ESP-IDF examples and fixtures with a compatible, verified
  ESP-IDF checkout. Vibe Mode currently requires ESP-IDF 6.2 or newer and the
  ESP32-S31 target.
- Preserve reviewed Vibe Mode binaries until their replacement passes manifest,
  layout, hash, and device validation.

## Device operations

Perform device operations only through the consuming workspace's `mosaico.py`
launcher. Do not directly invoke ESP-IDF or ESP-Iris device-write commands, and
do not erase credentials, identity, Vibe Mode data, or partitions without
explicit authorization.

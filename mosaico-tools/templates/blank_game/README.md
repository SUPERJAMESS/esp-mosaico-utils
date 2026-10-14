# blank_game

A minimal Raylib Lite game: the first frame is an empty black canvas. No example
gameplay, atlas, sounds, asset generation pipeline or game resource partition is
included. The shared native launcher presents the RGB565 frame through the selected
ESP-Mosaico Board and supplies touch, Iris and first-frame health handling.

From the workspace root:

```sh
python mosaico.py game sim --project projects/my_game
python mosaico.py game sim --project projects/my_game --headless --frames 120
python mosaico.py game build --project projects/my_game
python mosaico.py iris system-update --project projects/my_game
```

Run `python mosaico.py recover` before first installation on a blank or unverified
device. New applications, changed layouts or external resources use
`iris system-update`. The immutable Vibe Mode partitions and OTA routing are
already configured; the engine starts Iris and accepts the image after its first
frame. USB High-Speed remains owned by Iris. Physical input/display behavior
still requires device validation after Host checks.

- `components/app_game`: pure C state and gameplay rules, no graphics dependency.
- `components/app_game_runtime`: Engine input/lifecycle adapter, rendering and JSON.
- `components/app_game_runtime/private/game_config.h`: identity, pixel dimensions
  and tick frequency; never included by another component.
- `main/game_entry.c`: selects the adapter for the unchanged Engine module ABI.
- `game.sim.json`: explicit list of the same sources used by the Host module.

Dependencies flow `main -> app_game_runtime -> app_game`, with the Engine as a
platform dependency of the runtime. The model never depends on its caller or on
rendering. Public headers live under each component's `include/`; internal state
and configuration stay private. Component READMEs document contracts and usage.

Keep new application components cohesive and prefixed `app_`; keep dependencies
acyclic and entry points limited to composition/startup. Exchange handles and
value snapshots, never extern business globals or private structures. Keep public
API comments within three lines; put detailed contracts in the component README
and document units/ranges beside fields. Add an
abstraction only when a concrete independent responsibility requires it.
Third-party code retains upstream names. Pins and workspace overrides live in
`main/idf_component.yml` and the root CMake configuration. Apply local patches in
a dedicated dependency checkout selected by `override_path`; never edit
`managed_components/` in place. The prefix hook checks naming;
code review and behavior tests establish architectural and API correctness.

Git, editor and hook entry files stay at the root; format rules, project policy
and hook setup instructions live in `tools/quality/`.

Host state exposes `phase`, `tick`, `pointer_x`, `pointer_y`, `pointer_down` and
`state_hash` for automation. Touch/pointer input records coordinates without
drawing a cursor. Pause/resume/reset are wired for Host controls and replay;
there are no gameplay keyboard bindings yet. Add each new input behavior to the
shared model and map it from the device and Host adapters.

Host needs a C compiler and Pillow, and uses the same C renderer as firmware.
Firmware needs the pinned workspace ESP-IDF and initialized BSP, engine and utils.
Before using any other Raylib API, check the engine's supported public headers.

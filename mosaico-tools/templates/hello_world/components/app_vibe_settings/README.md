# app_vibe_settings

Owns the Vibe Mode `sysmeta` contract: `vibe_prefs` brightness, timeout, rotate,
language and haptics, plus the `shared` namespace and the one STA Wi-Fi page.
It does not draw. The scene lives in `app_hello/ui/main.json`. `main` routes
view snapshots to the UI and brightness or rotation requests to `app_display`.

Missing keys use the Vibe Mode fallback and are not written until the user
changes that control. `open_confirm` and factory reset stay in Vibe Mode.
Volume is the application NVS namespace `hello`, key `volume` (steps of 20,
missing shows 80). It is not written to `vibe_prefs`. Hello World has no
speaker; the stored step is shown on the volume page. Motor strength is not
on this page. A motor strength of 0 calls `app_vibe_settings_set_haptics(false)`.
Shared keys remain in the view. The Hello World scene does not show them.

All five group flags false means every group is on. A settings task of 6144
bytes is the only writer. UI callbacks only queue. Scan records are heap
allocated. The password is never logged.

Host simulation does not compile this component. The PC adapter records the
same commands in memory and repaints. Backlight, IMU, the motor and a live
scan remain device-only.

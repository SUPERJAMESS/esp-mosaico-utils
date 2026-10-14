# app_settings

Owns Vibe Mode preferences and shared information in `sysmeta/vibe_prefs` and
`sysmeta/shared`. Initialize after system metadata and before network startup.
UI submits bounded requests with `vibe_settings_request` and polls snapshots;
NVS writes run in a worker, never the render task. Values are not logged.

Reset clears only `wifi`, `shared`, and `vibe_prefs`, with a durable reset marker
and a Vibe Mode boot request. It also cancels the single deferred
`iris_bridge_v2/pending_boot` record so a remote launch cannot bypass first-run
setup. Startup resumes interrupted resets. NAND, application
images, the boot identity, Iris pairing credentials, and device identity remain
untouched. The UI requires explicit confirmation before submitting reset.

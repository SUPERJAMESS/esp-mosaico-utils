# GSP application preview

From a workspace use `python mosaico.py project sim --project projects/my_app`.
The project can also be selected from cwd, a configured default, or the sole app.
There is no hard-coded Hello World fallback. Install the application's pinned
ESP-GSP 1.5.1 component through an IDF build/reconfigure before native preview.

The runner selects the scene owner's `components/<name>/host/CMakeLists.txt`
backend, falling back to `pc/CMakeLists.txt` for older applications. It compiles
shared C UI logic into the native backend; the matching official simulator renders it.
The component's ESP-IDF CMake entry registers device sources explicitly and does
not compile files under `host/`.
Use --interactive for browser preview, or --headless --duration 3 for a bounded
smoke run. Backend build artifacts are local to the application in build-sim/.

--scene PATH, --scene-only, --frames, --fps and --dump-ppm select scene-only rendering.
These modes do not execute application C callbacks. The public product command
accepts --dump-ppm PATH; direct `run.py --project PATH` or an explicit scene file
is also supported. Tool downloads use user caches and do not rely on another app.
For a direct backend invocation, `run.py --pc-project PATH` accepts either layout;
dependency lookup and `build-sim/` remain rooted at the application directory.

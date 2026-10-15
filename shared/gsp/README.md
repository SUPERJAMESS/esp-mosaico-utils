# GSP host tools

This module downloads, caches and locates GSPC/simulator executables. It has no
application, firmware, ESP-IDF or Python GSP package dependency. Release pins
belong to callers: applications use `mosaico-tools/gsp-toolchain.json`.
Vibe Mode firmware does not use GSP or this compiler bootstrap.

## Minimal usage

From the utils root:

```sh
python shared/gsp/fetch.py --tool-version 0.5.0
python shared/gsp/fetch.py --toolchain mosaico-tools/gsp-toolchain.json --sim
```

For ESP-IDF, before including its project entry point:

```cmake
set(MOSAICO_GSPC_VERSION "0.5.0")
include("${MOSAICO_UTILS_ROOT}/shared/gsp/compiler.cmake")
```

Alternatively supply `MOSAICO_GSP_TOOLCHAIN`, a path to JSON containing `gspc`
and/or `sim` version strings. An explicit version takes precedence. The shared
entry has no default version. `MOSAICO_GSPC_PYTHON` selects the interpreter.
Applications retain their small tools-owned CMake entry to select their pins.

## Contract

- `resolve_release(product=..., version=..., env_var=..., cache_dir=None)`
  returns a borrowed `Path` to a cached executable. Products are `gspc` and `sim`;
  callers select the matching override variable (`GSPC_EXECUTABLE` or
  `GSP_SIM_EXECUTABLE`). Versions are release strings.
- Executable overrides must be existing absolute paths. Otherwise acquisition
  uses the caller's cache, the matching cache environment variable, or a
  versioned `esp-mosaico` cache under XDG/home. The final fallback is local to
  this shared module. Callers must not delete returned cached executables.
- Calls block on filesystem/network I/O. Each HTTP request has a 120-second
  timeout; there is no overall deadline. Callers serialize writes to the same
  cache. No thread safety is promised for concurrent acquisition.
- Invalid configuration raises `ValueError` or `RuntimeError`; filesystem and
  network errors propagate. CLI success prints only the executable path and
  exits 0; acquisition failure exits 1, invalid arguments exit 2.
- CMake preserves an explicit/cached executable, then an environment override,
  before resolving the requested release. Resolution failure stops configure.

Application-side component discovery and the simulator runner stay in tools.
They delegate acquisition here and keep their own fallback version policy.

# Shared host capabilities

This directory owns GSP tool acquisition and Ideas display-resource generation.
Mosaico Tools uses the GSP acquisition module; tools and Vibe Mode authoring
entry points share Ideas generation. Both consume the same utils revision,
without separate installation or releases. Vibe Mode firmware uses its own
lightweight UI and does not load GSP.

| Directory | Responsibility | Dependencies |
| --- | --- | --- |
| [gsp](gsp/README.md) | Acquire selected GSP tools and expose them to CMake | Python 3.8+ standard library; CMake for build integration |
| [ideas](ideas/README.md) | Generate display/1, MIFF/1 and optional icon bytes | Optional host imaging and text-shaping libraries |

Shared modules do not import consumers or locate their private files. Callers
own product versions, input paths and output destinations. Source-tree entry
points add the utils root to Python's module search path once; library calls
never temporarily modify it.

Product contracts, Recovery ABI, application adapters and boot policy remain
owned by Vibe Mode. Templates and application integration remain in Mosaico Tools.
New shared capabilities need real consumers and a small, documented interface.

From the utils root:

```sh
python -m pip install pytest -r shared/ideas/requirements.txt
python -m pytest -q shared
```

Consumer integration tests remain in their respective suites. CI runs the shared
suite on Linux, macOS and Windows with Python 3.8 and 3.12.

# Ideas display resources

This host library shapes text and generates display/1 JSON, MIFF/1 font subsets
and optional PNG icons. It has no ESP-IDF, GSP, CLI or firmware-source dependency.
Vibe Mode owns the [device format](../../esp-mosaico-vibe-mode/docs/ideas-display-format.md).

Install `requirements.txt` and system libraqm >= 0.9. Importing the public
package does not load these optional libraries; generation does.

## Minimal usage

From the utils root, or with that root on the Python module search path:

```python
from pathlib import Path
from shared.ideas import generate_resources

files = generate_resources(
    name="Weather", description="Clear skies.", locale="en",
    font=Path("/path/to/font.ttf"),
)
```

The caller writes returned bytes or inserts them into its archive. The firmware's
`prepare_idea_presentation.py` remains a thin file-output command.

## Contract

- Arguments are keyword-only. `name` is nonempty and at most 128 UTF-8 bytes;
  `description` is at most 512 bytes. Only newline controls are accepted.
  `locale` is a nonempty ASCII language tag of at most 23 bytes.
- `font` and optional `icon` are caller-supplied `Path` values. The font must
  cover the text. Text must fit the device's title/body areas; overflow and
  unsupported glyphs fail explicitly. Font licensing remains with the caller.
- Icons must be static JPEG/PNG/GIF/WebP, at most 10 MiB and 4096 pixels per
  axis. Output is centered in a 64 × 64 RGBA PNG. Total output is at most
  1 MiB. Existing format limits and serialization remain unchanged.
- The result is an owned `dict[str, bytes]` containing `display.json`,
  `title.mif`, `body.mif` and optionally `icon.png`. Inputs are read only;
  the function does not write files, contact services or add application identity.
- Generation is synchronous without a timeout. Each call owns its rendering
  state; concurrency depends on native shaping libraries, so no cross-thread
  rendering guarantee is made.
- Invalid inputs/device limits raise `ValueError`; I/O errors propagate.
  Missing Python dependencies raise `ImportError`; missing libraqm raises
  `RuntimeError`. CLI entry points translate errors into their own diagnostics.

Implementation modules are internal. Consumers use `shared.ideas.generate_resources`.
Firmware integration tests roundtrip resources through the actual C readers.

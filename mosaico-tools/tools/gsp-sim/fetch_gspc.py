#!/usr/bin/env python3
"""Select application GSP versions and delegate acquisition to shared.gsp."""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

# Source-tree entry point; no installation is required by consuming workspaces.
UTILS_ROOT = Path(__file__).resolve().parents[3]
if str(UTILS_ROOT) not in sys.path:
    sys.path.insert(0, str(UTILS_ROOT))
from shared.gsp.fetch import resolve_release, toolchain_version  # noqa: E402

TOOLCHAIN = UTILS_ROOT / "mosaico-tools/gsp-toolchain.json"
MANAGED_GSP = "espressif__esp-gsp"
PINNED_GSP_VERSION = toolchain_version(TOOLCHAIN, "sim")
PINNED_GSPC_VERSION = toolchain_version(TOOLCHAIN, "gspc")


def _managed_gsp_dir(project_dir: Path) -> Path:
    return project_dir / "managed_components" / MANAGED_GSP


def resolve_gsp_root(project_dir: Path | None = None) -> Path | None:
    """Locate the downloaded espressif/esp-gsp component, if present."""
    configured = os.environ.get("ESP_GSP_COMPONENT_DIR")
    if configured:
        path = Path(configured).expanduser()
        if path.is_dir():
            return path

    candidates: list[Path] = []
    if project_dir is not None:
        project = project_dir.resolve()
        candidates.append(_managed_gsp_dir(project))
        if project.name == "pc":
            candidates.append(_managed_gsp_dir(project.parent))

    seen: set[Path] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if (resolved / "idf_component.yml").is_file():
            return resolved
    return None


def gspc_version(gsp_root: Path | None = None) -> str:
    root = gsp_root or resolve_gsp_root()
    if root is not None:
        marker = root / ".gspc_version"
        if marker.is_file():
            text = marker.read_text(encoding="utf-8").strip()
            if text:
                return text
    return PINNED_GSPC_VERSION


def gsp_component_version(gsp_root: Path | None = None) -> str:
    root = gsp_root or resolve_gsp_root()
    if root is not None:
        manifest = root / "idf_component.yml"
        if manifest.is_file():
            for line in manifest.read_text(encoding="utf-8").splitlines():
                if line.startswith("version:"):
                    return line.split(":", 1)[1].strip().strip("'\"")
    return PINNED_GSP_VERSION


def resolve_gspc(
    cache_dir: Path | None = None,
    gsp_root: Path | None = None,
) -> Path:
    return resolve_release(
        product="gspc",
        version=gspc_version(gsp_root),
        env_var="GSPC_EXECUTABLE",
        cache_dir=cache_dir,
    )


def resolve_sim(
    cache_dir: Path | None = None,
    gsp_root: Path | None = None,
) -> Path:
    return resolve_release(
        product="sim",
        version=gsp_component_version(gsp_root),
        env_var="GSP_SIM_EXECUTABLE",
        cache_dir=cache_dir,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--sim", action="store_true", help="print the simulator path")
    parser.add_argument(
        "--tool-version", help="resolve this release instead of the default tool pin",
    )
    parser.add_argument(
        "--pinned", action="store_true",
        help="use workspace tool pins, ignoring an older installed component during upgrades",
    )
    args = parser.parse_args()
    try:
        if args.pinned or args.tool_version:
            path = resolve_release(
                product="sim" if args.sim else "gspc",
                version=args.tool_version or (
                    PINNED_GSP_VERSION if args.sim else PINNED_GSPC_VERSION),
                env_var="GSP_SIM_EXECUTABLE" if args.sim else "GSPC_EXECUTABLE",
                cache_dir=args.output_dir,
            )
        else:
            path = resolve_sim(args.output_dir) if args.sim else resolve_gspc(args.output_dir)
    except Exception as error:
        print(f"{'sim' if args.sim else 'gspc'}: {error}", file=sys.stderr)
        return 1
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

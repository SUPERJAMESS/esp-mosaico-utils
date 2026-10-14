#!/usr/bin/env python3
"""Validate monorepo paths, versions, and retired repository references."""

from pathlib import Path
import json
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
IRIS = ROOT / "ESP-Iris"
RECOVERY = ROOT / "esp-mosaico-vibe-mode"


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)


def main() -> int:
    errors = 0

    required = (
        IRIS / "components" / "esp_iris" / "idf_component.yml",
        IRIS / "components" / "esp_iris" / "tools" / "system_update_bundle.py",
        ROOT / "mosaico-tools" / "mosaico.py",
        ROOT / "mosaico-tools" / "tools" / "mosaico_cli" / "__init__.py",
        RECOVERY / "firmware" / "vibe-mode" / "prebuilt" / "recovery" / "manifest.json",
    )
    for path in required:
        if not path.is_file():
            fail(f"required file is missing: {path.relative_to(ROOT)}")
            errors += 1

    manifest = RECOVERY / "firmware" / "vibe-mode" / "main" / "idf_component.yml"
    override = re.search(
        r"^\s*override_path:\s*(\S+)\s*$",
        manifest.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    expected_component = (IRIS / "components" / "esp_iris").resolve()
    if override is None or (manifest.parent / override.group(1)).resolve() != expected_component:
        fail("Recovery component manifest does not resolve the sibling ESP-Iris checkout")
        errors += 1

    iris_manifest = (IRIS / "components" / "esp_iris" / "idf_component.yml").read_text(
        encoding="utf-8"
    )
    if not re.search(r'^version:\s*"0\.2\.0"\s*$', iris_manifest, re.MULTILINE):
        fail("ESP-Iris release version is not 0.2.0")
        errors += 1
    if "path: ESP-Iris/components/esp_iris" not in iris_manifest:
        fail("ESP-Iris repository_info.path is not monorepo-relative")
        errors += 1

    gateway_version = (
        IRIS / "components" / "esp_iris" / "tools" / "iris_gateway" / "__init__.py"
    ).read_text(encoding="utf-8")
    if '__version__ = "0.2.0"' not in gateway_version:
        fail("ESP-Iris Gateway release version is not 0.2.0")
        errors += 1

    frontend_root = IRIS / "components" / "esp_iris" / "tools" / "frontend"
    for name in ("package.json", "package-lock.json"):
        package = json.loads((frontend_root / name).read_text(encoding="utf-8"))
        if package.get("version") != "0.2.0":
            fail(f"ESP-Iris Workbench release version in {name} is not 0.2.0")
            errors += 1
        if name == "package-lock.json" and package.get("packages", {}).get("", {}).get(
            "version"
        ) != "0.2.0":
            fail("ESP-Iris Workbench root lock-package version is not 0.2.0")
            errors += 1

    tools_version = (ROOT / "mosaico-tools" / "tools" / "mosaico_cli" / "__init__.py").read_text(
        encoding="utf-8"
    )
    if '__version__ = "0.2.0"' not in tools_version:
        fail("ESP-Mosaico Tools release version is not 0.2.0")
        errors += 1

    retired = (
        "github.com/lisir233/ESP-Iris",
        "github.com/lisir233/esp-mosaico-tools",
        "submodule/esp-mosaico-tools",
        "pinned ESP-Iris submodule",
        "tools-owned Recovery",
    )
    excluded = {
        Path("MIGRATION.md"),
        Path("tools/check_repository_layout.py"),
    }
    for path in ROOT.rglob("*"):
        relative = path.relative_to(ROOT)
        if (
            not path.is_file()
            or relative in excluded
            or ".git" in relative.parts
            or "__pycache__" in relative.parts
            or "tests" in relative.parts
            and "fixtures" in relative.parts
        ):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for value in retired:
            if value in text:
                fail(f"retired reference {value!r} remains in {relative}")
                errors += 1

    for path in (IRIS / ".gitmodules", RECOVERY / ".gitmodules"):
        if path.exists():
            fail(f"nested submodule metadata remains: {path.relative_to(ROOT)}")
            errors += 1

    if errors:
        return 1
    print("repository layout and 0.2.0 release sources are consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

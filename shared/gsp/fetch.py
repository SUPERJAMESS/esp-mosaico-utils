#!/usr/bin/env python3
"""Resolve caller-selected GSP host tools; no product versions are selected here."""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
LICENSE_NAME = "THIRD_PARTY_LICENSES.txt"


def toolchain_version(path: Path, product: str) -> str:
    """Read a caller-owned release pin; raise ValueError for a missing/invalid pin."""
    pins = json.loads(path.read_text(encoding="utf-8"))
    version = pins.get(product) if isinstance(pins, dict) else None
    if not isinstance(version, str) or not re.fullmatch(r"[0-9][0-9A-Za-z.+-]*", version):
        raise ValueError(f"Missing or invalid {product} version in {path}")
    return version


def host_os_arch() -> tuple[str, str]:
    system = platform.system()
    machine = platform.machine().lower()
    if machine in {"amd64", "x86_64"}:
        arch = "x86_64"
    elif machine in {"aarch64", "arm64"}:
        arch = "aarch64"
    else:
        raise RuntimeError(f"unsupported host architecture: {machine}")
    if system == "Linux":
        return "linux", arch
    if system == "Darwin":
        return "macos", "universal2"
    if system == "Windows":
        return "windows", arch
    raise RuntimeError(f"unsupported GSP host: {system}/{machine}")


def archive_name(product: str, version: str) -> str:
    os_name, arch = host_os_arch()
    suffix = "zip" if os_name == "windows" else "tar.gz"
    return f"{product}-{version}-{os_name}-{arch}.{suffix}"


def default_cache_dir(kind: str, version: str) -> Path:
    configured = os.environ.get("GSPC_CACHE_DIR" if kind == "gspc" else "GSP_SIM_CACHE_DIR")
    if configured:
        path = Path(configured).expanduser()
        path.mkdir(parents=True, exist_ok=True)
        return path
    xdg = os.environ.get("XDG_CACHE_HOME")
    candidates = []
    if xdg:
        candidates.append(Path(xdg) / "esp-mosaico" / kind / version)
    candidates.append(Path.home() / ".cache" / "esp-mosaico" / kind / version)
    candidates.append(TOOLS_DIR / ".cache" / kind / version)
    for path in candidates:
        try:
            path.mkdir(parents=True, exist_ok=True)
            return path
        except OSError:
            continue
    raise RuntimeError(f"cannot create a {kind} cache directory")


def download(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_file():
        return
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        suffix=".download",
        dir=destination.parent,
    )
    temporary = Path(temporary_name)
    try:
        request = urllib.request.Request(
            url, headers={"User-Agent": "esp-mosaico-vibe-gsp/1"}
        )
        with os.fdopen(fd, "wb") as output:
            with urllib.request.urlopen(request, timeout=120) as response:
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, destination)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


def extract_binary(archive: Path, binary: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"{binary}-extract-") as directory:
        staging = Path(directory)
        if archive.suffix == ".zip":
            with zipfile.ZipFile(archive) as bundle:
                bundle.extractall(staging)
        else:
            with tarfile.open(archive, "r:gz") as bundle:
                bundle.extractall(staging)
        matches = [path for path in staging.rglob(binary) if path.is_file()]
        if not matches:
            raise RuntimeError(f"{binary} missing from {archive.name}")
        shutil.copy2(matches[0], destination)
    if os.name != "nt":
        destination.chmod(0o755)


def resolve_release(
    *,
    product: str,
    version: str,
    env_var: str,
    cache_dir: Path | None = None,
) -> Path:
    """Return a cached tool path for an explicit release; may block on downloads.
    Callers serialize cache writes; invalid input and acquisition errors propagate.
    """
    if product not in {"gspc", "sim"} or not re.fullmatch(r"[0-9][0-9A-Za-z.+-]*", version):
        raise ValueError("Expected gspc/sim and a valid release version")
    configured = os.environ.get(env_var)
    if configured:
        executable = Path(configured).expanduser()
        if not executable.is_absolute():
            raise RuntimeError(f"{env_var} must be an absolute path")
        if not executable.is_file():
            raise RuntimeError(f"{env_var} is not a file: {executable}")
        return executable

    cache = cache_dir or default_cache_dir(product, version)
    os_name, _ = host_os_arch()
    binary_name = product + (".exe" if os_name == "windows" else "")
    binary = cache / binary_name
    if binary.is_file():
        return binary
    name = archive_name(product, version)
    base = f"https://dl.espressif.com/AE/gsp/{product}/v{version}/"
    archive = cache / name
    download(base + name, archive)
    download(base + LICENSE_NAME, cache / LICENSE_NAME)
    extract_binary(archive, binary_name, binary)
    return binary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    version = parser.add_mutually_exclusive_group(required=True)
    version.add_argument("--tool-version", help="explicit tool release version")
    version.add_argument("--toolchain", type=Path, help="caller-owned JSON with gspc/sim release pins")
    parser.add_argument("--sim", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    product = "sim" if args.sim else "gspc"
    try:
        pin = args.tool_version or toolchain_version(args.toolchain, product)
        executable = resolve_release(product=product, version=pin,
            env_var="GSP_SIM_EXECUTABLE" if args.sim else "GSPC_EXECUTABLE", cache_dir=args.output_dir)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"{product}: {error}", file=sys.stderr)
        return 1
    print(executable)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

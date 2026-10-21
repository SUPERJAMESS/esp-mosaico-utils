"""Make gspc's escaped Windows canonical paths consumable by CMake/Ninja."""
from __future__ import annotations

import argparse
import re
from pathlib import Path


def _paths(value: str) -> list[str]:
    paths, token = [], []
    index = 0
    while index < len(value):
        char = value[index]
        if char == "\\" and index + 1 < len(value):
            index += 1
            char = value[index]
            if char == "\n":
                index += 1
                continue
            token.append(char)
        elif char.isspace():
            if token:
                paths.append("".join(token))
                token = []
        else:
            token.append(char)
        index += 1
    if token:
        paths.append("".join(token))
    return paths


def _escape(path: str) -> str:
    if path.startswith("\\\\?\\UNC\\"):
        path = "\\\\" + path[8:]
    elif path.startswith("\\\\?\\"):
        path = path[4:]
    path = path.replace("\\", "/")
    # gspc emits Make-style depfiles; preserve whitespace/metacharacters after
    # removing the Windows extended-length prefix and escaped separators.
    return path.replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ")


def normalize(text: str) -> str:
    separator = re.search(r":\s", text)
    if separator is None:
        raise ValueError("GSP depfile has no target/dependency separator")
    targets = _paths(text[:separator.start()])
    dependencies = _paths(text[separator.end():])
    if not targets:
        raise ValueError("GSP depfile has no target")
    return " ".join(map(_escape, targets)) + ": " + " ".join(map(_escape, dependencies)) + "\n"


def normalize_file(path: Path) -> bool:
    contents = path.read_text(encoding="utf-8")
    # Other hosts and already-normalized files pass through byte-for-byte.
    if "\\\\\\\\?\\\\" in contents:
        path.write_bytes(normalize(contents).encode("utf-8"))
        return True
    return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("depfile", type=Path)
    parser.add_argument("--repair-ninja-cache", type=Path)
    args = parser.parse_args()
    if normalize_file(args.depfile) and args.repair_ninja_cache is not None:
        # An older Ninja deps log still holds the malformed paths and can fail
        # before running the corrected command. Discard only this regenerable
        # dependency index; keep objects, configuration and build artifacts.
        cache = args.repair_ninja_cache / ".ninja_deps"
        if cache.exists():
            cache.unlink()
            print("Removed stale Ninja dependency index after repairing Windows GSP paths; one rebuild is required.")

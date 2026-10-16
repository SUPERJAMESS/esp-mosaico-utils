from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any

from .chip_targets import ChipTarget, target_from_chip_id

_PANIC_RE = re.compile(r"^Panic reason:\s*(.+)$", re.MULTILINE)
_TASK_RE = re.compile(r"^Crashed task handle:.*?name:\s*'([^']+)'", re.MULTILINE)
_FRAME_RE = re.compile(
    r"^#(?P<index>\d+)\s+(?:0x[0-9a-fA-F]+\s+in\s+)?"
    r"(?P<function>[^\s(]+)"
)
_SOURCE_RE = re.compile(r"\s+at\s+(?P<file>.+):(?P<line>\d+)\s*$")
_CONTEXT_BOOT_RE = re.compile(r"IRIS_CONTEXT_BOOT=(\d+)")


def _inside(path: str, root: str) -> bool:
    try:
        normalized_path = os.path.normcase(os.path.abspath(path))
        normalized_root = os.path.normcase(os.path.abspath(root))
        return os.path.commonpath((normalized_path, normalized_root)) == normalized_root
    except ValueError:
        return False


def _tooling(target: ChipTarget) -> tuple[pathlib.Path, pathlib.Path, str] | None:
    idf_value = os.environ.get("ESP_IRIS_IDF_PATH") or os.environ.get("IDF_PATH")
    if not idf_value:
        return None
    idf = pathlib.Path(idf_value).expanduser().resolve()
    script = idf / "components" / "espcoredump" / "espcoredump.py"
    python = pathlib.Path(
        os.environ.get("ESP_IRIS_IDF_PYTHON") or sys.executable
    ).expanduser().absolute()
    gdb_value = os.environ.get("ESP_IRIS_GDB") or target.gdb
    gdb = shutil.which(gdb_value)
    if not script.is_file() or not python.is_file() or gdb is None:
        return None
    return python, script, gdb


def _frames(output: str) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for line in output.splitlines():
        match = _FRAME_RE.match(line)
        if match is None:
            continue
        item: dict[str, Any] = {
            "index": int(match.group("index")),
            "function": match.group("function"),
        }
        # Panic arguments can contain "abort() was called at PC ...".
        # GDB appends the actual source location after the final " at ".
        source_at = line.rfind(" at ")
        source = _SOURCE_RE.match(line, source_at) if source_at >= 0 else None
        if source is not None:
            item.update(
                file=source.group("file"), line=int(source.group("line"))
            )
        if item not in result:
            result.append(item)
    return result[:64]


def decode_coredump(
    core_path: pathlib.Path,
    firmware_artifact: dict[str, Any],
    *,
    expected_failed_boot_id: int,
    timeout: float = 90,
) -> tuple[dict[str, Any], bytes]:
    """Decode one retained dump with its exact archived ELF.

    The raw decoder output is returned separately so callers can preserve it
    without inflating structured crash metadata.
    """

    try:
        target = target_from_chip_id(firmware_artifact.get("chip_id"))
    except ValueError as error:
        return ({
            "status": "unavailable", "reason": str(error),
            "source_location_confirmed": False,
            "incident_identity_confirmed": False,
        }, b"")
    tooling = _tooling(target)
    if tooling is None:
        return ({
            "status": "unavailable",
            "reason": f"ESP-IDF coredump decoder or GDB for {target.name} is unavailable",
            "source_location_confirmed": False,
            "incident_identity_confirmed": False,
        }, b"")
    python, script, gdb = tooling
    folder = pathlib.Path(str(firmware_artifact["path"]))
    elf_name = firmware_artifact.get("files", {}).get(
        "firmware.elf", "firmware.elf"
    )
    elf = folder / str(elf_name)
    if not elf.is_file():
        return ({
            "status": "unavailable",
            "reason": "matching archived ELF is missing",
            "source_location_confirmed": False,
            "incident_identity_confirmed": False,
        }, b"")

    with tempfile.TemporaryDirectory(prefix="esp-iris-coredump-") as temporary:
        normalized = pathlib.Path(temporary) / "core.elf"
        command = [
            str(python), str(script), "--chip", target.name, "info_corefile",
            "--core", str(core_path), "--core-format", "raw",
            "--save-core", str(normalized), "--gdb", gdb, str(elf),
        ]
        try:
            completed = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return ({
                "status": "failed",
                "reason": str(error),
                "source_location_confirmed": False,
                "incident_identity_confirmed": False,
            }, str(error).encode("utf-8", errors="replace"))

        output = completed.stdout or ""
        if completed.returncode or not normalized.is_file():
            return ({
                "status": "failed",
                "reason": f"esp-coredump exited with status {completed.returncode}",
                "source_location_confirmed": False,
                "incident_identity_confirmed": False,
            }, output.encode("utf-8", errors="replace"))

        try:
            gdb_result = subprocess.run(
                [
                    gdb, "--batch", "--quiet", "--nx", str(elf),
                    "--core", str(normalized),
                    "-ex", (
                        'printf "IRIS_CONTEXT_BOOT=%llu\\n", '
                        "(unsigned long long)g_iris_crash_context.boot_id"
                    ),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return ({
                "status": "failed",
                "reason": str(error),
                "source_location_confirmed": False,
                "incident_identity_confirmed": False,
            }, (output + "\n" + str(error)).encode("utf-8", errors="replace"))
        context_output = gdb_result.stdout or ""
        context_match = _CONTEXT_BOOT_RE.search(context_output)
        context_boot_id = int(context_match.group(1)) if context_match else 0
        incident_confirmed = bool(
            expected_failed_boot_id
            and context_boot_id == expected_failed_boot_id
        )
        parsed_frames = _frames(output)
        idf_root = os.environ.get("ESP_IRIS_IDF_PATH") or os.environ.get(
            "IDF_PATH", ""
        )
        source = next(
            (
                frame for frame in parsed_frames
                if frame.get("file")
                and not _inside(str(frame["file"]), idf_root)
            ),
            None,
        )
        panic = _PANIC_RE.search(output)
        task = _TASK_RE.search(output)
        diagnosis: dict[str, Any] = {
            "status": "succeeded",
            "panic_reason": panic.group(1).strip() if panic else "",
            "crashed_task": task.group(1) if task else "",
            "frames": parsed_frames,
            "source_location": source,
            "source_location_confirmed": source is not None,
            "context_boot_id": context_boot_id,
            "expected_failed_boot_id": expected_failed_boot_id,
            "incident_identity_confirmed": incident_confirmed,
        }
        if not incident_confirmed:
            diagnosis["reason"] = (
                "Core Dump Boot ID does not match the retained failed Boot ID"
            )
        combined = output
        if context_output:
            combined += "\n================ IRIS INCIDENT CONTEXT ================\n"
            combined += context_output
        return diagnosis, combined.encode("utf-8", errors="replace")

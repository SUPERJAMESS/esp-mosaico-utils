"""Run processes, store logs, and resolve the ESP-IDF environment."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
import uuid
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .build_progress import decode_progress
from .errors import BuildError, DeviceError, EnvironmentError
from .host import (
    HostEnvironmentError,
    configdep_environment,
    prepare_idf_environment,
    valid_idf_path,
)
from .process_output import stream_process
from .workspace import WorkspaceConfig


@dataclass
class RunContext:
    workspace: WorkspaceConfig
    action: str
    verbose: bool = False
    json_output: bool = False

    def __post_init__(self) -> None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        base = self.workspace.run_dir
        suffix = 0
        while True:
            name = f"{stamp}-{self.action}"
            if suffix:
                name += f"-{suffix}"
            candidate = base / name
            try:
                # mkdir is the atomic allocation step.  An exists()/mkdir()
                # pair races when two mosaico processes start in one second.
                candidate.mkdir(parents=True, exist_ok=False)
                break
            except FileExistsError:
                suffix += 1
        self.directory = candidate
        self.log_path = candidate / "raw.log"
        self.started_monotonic = time.monotonic()

    @property
    def repository(self) -> Path:
        """Compatibility alias for subprocesses whose cwd is the workspace."""

        return self.workspace.root

    def note(self, message: str) -> None:
        with self.log_path.open("a", encoding="utf-8") as stream:
            stream.write(message.rstrip() + "\n")

    def status(self, message: str) -> None:
        elapsed = time.monotonic() - self.started_monotonic
        line = f"[+{elapsed:6.1f}s] {message}"
        self.note(line)
        if not self.json_output:
            print(line, flush=True)

    def run(
        self,
        argv: Sequence[str | os.PathLike[str]],
        *,
        timeout: float | None = None,
        cwd: Path | None = None,
        check: bool = False,
        env: dict[str, str] | None = None,
        input_text: str | None = None,
        output_status: Callable[[str], str | None] | None = None,
        sensitive_output: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        command = [os.fspath(item) for item in argv]
        self.note(
            "$ [sensitive command omitted]"
            if sensitive_output else "$ " + " ".join(command)
        )
        try:
            if output_status is None:
                result = subprocess.run(
                    command,
                    cwd=cwd,
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    input=input_text,
                    timeout=timeout,
                    check=False,
                )
                self.note("[sensitive output omitted]" if sensitive_output else result.stdout or "")
            else:
                if input_text is not None:
                    raise ValueError("streaming commands do not accept stdin input")
                result = self._run_streaming(
                    command,
                    timeout=timeout,
                    cwd=cwd,
                    env=env,
                    output_status=output_status,
                    sensitive_output=sensitive_output,
                )
        except (OSError, subprocess.TimeoutExpired) as error:
            self.note(f"process error: {error}")
            raise
        self.note(f"exit_code={result.returncode}")
        if check and result.returncode:
            error = subprocess.CalledProcessError(result.returncode, command, output=result.stdout)
            error.stdout_path = getattr(result, "stdout_path", None)
            error.stdout_truncated = getattr(result, "stdout_truncated", False)
            raise error
        return result

    def _run_streaming(
        self,
        command: list[str],
        *,
        timeout: float | None,
        cwd: Path | None,
        env: dict[str, str] | None,
        output_status: Callable[[str], str | None],
        sensitive_output: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        path = None if sensitive_output else self.directory / f"stdout-{uuid.uuid4().hex}.log"
        self.note("[sensitive output omitted]" if path is None else f"complete stdout: {path}")

        def report(line: str) -> None:
            if not sensitive_output:
                message = output_status(line)
                if message:
                    self.status(message)

        result = stream_process(command, path=path, timeout=timeout, cwd=cwd, env=env, on_line=report)
        if result.progress_dropped:
            self.note(f"progress notifications skipped: {result.progress_dropped}; complete output: {path}")
        return result


def resolve_idf_path(workspace: WorkspaceConfig, project: Path | None = None) -> Path:
    candidates: list[Path] = []
    environment = os.environ.get("IDF_PATH")
    if environment:
        candidates.append(Path(environment))
    for owner in (project, workspace.recovery_project):
        if owner is None:
            continue
        description = owner / "build" / "project_description.json"
        try:
            value = json.loads(description.read_text(encoding="utf-8"))
            if value.get("idf_path"):
                candidates.append(Path(value["idf_path"]))
        except (OSError, ValueError, TypeError):
            pass
    for candidate in candidates:
        resolved = candidate.expanduser().resolve()
        if valid_idf_path(resolved):
            return resolved
    raise EnvironmentError(
        "No compatible ESP-IDF environment was found. Set IDF_PATH or complete "
        "environment verification first."
    )


def build_application(context: RunContext, project: Path) -> None:
    runner = context.workspace.build_runner
    if not runner.is_file():
        raise EnvironmentError(f"The configured build runner is unavailable: {runner}")
    idf_path = resolve_idf_path(context.workspace, project)
    context.status(f"build: preparing {project}")
    result = context.run(
        [
            sys.executable,
            runner,
            "--project",
            project,
            "--idf-path",
            idf_path,
            "--log-dir",
            context.directory / "build",
            "build",
            "--progress",
        ],
        timeout=3600,
        cwd=context.workspace.root,
        env=configdep_environment(context.workspace.configdep),
        output_status=_build_progress_parser(),
    )
    if result.returncode:
        diagnostic = "\n".join(
            line
            for line in (result.stdout or "").splitlines()
            if decode_progress(line) is None
        ).strip()
        raise BuildError(
            "Application build failed.",
            details={
                "diagnostic": diagnostic,
                "log": str(context.log_path),
                "build_log_dir": str(context.directory / "build"),
            },
        )


def _build_progress_parser() -> Callable[[str], str | None]:
    def parse(raw_line: str) -> str | None:
        message = decode_progress(raw_line)
        return f"build: {message}" if message else None

    return parse


def _idf_progress_parser() -> Callable[[str], str | None]:
    ansi = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
    ninja_progress = re.compile(r"^\[\s*(\d+)\s*/\s*(\d+)\s*\]")
    flash_progress = re.compile(
        r"Writing at (0x[0-9a-fA-F]+).*?\(\s*(\d+)\s*%\s*\)"
    )
    last_ninja_total = 0
    last_ninja_bucket = -1
    last_flash_percent = -1

    def parse(raw_line: str) -> str | None:
        nonlocal last_ninja_total, last_ninja_bucket, last_flash_percent
        line = ansi.sub("", raw_line).strip()
        if not line:
            return None

        ninja = ninja_progress.match(line)
        if ninja:
            completed, total = (int(value) for value in ninja.groups())
            percent = min(100, completed * 100 // max(total, 1))
            bucket = percent // 10
            if total != last_ninja_total:
                last_ninja_total = total
                last_ninja_bucket = -1
            if bucket != last_ninja_bucket or completed == total:
                last_ninja_bucket = bucket
                return f"idf: building {percent}% ({completed}/{total})"
            return None

        writing = flash_progress.search(line)
        if writing:
            address, percent_text = writing.groups()
            percent = int(percent_text)
            if percent < last_flash_percent:
                last_flash_percent = -1
            if percent != last_flash_percent:
                last_flash_percent = percent
                return f"flash: writing {percent}% at {address}"
            return None

        lower = line.lower()
        if line.startswith("Executing action:"):
            return f"idf: {line}"
        if "validating the reviewed recovery bundle" in lower:
            return "bundle: validating reviewed Vibe Mode images and manifest"
        if "building an unreviewed recovery candidate bundle" in lower:
            return "bundle: building current-source Vibe Mode candidate"
        if line.startswith("recovery bundle ready:"):
            return "bundle: Vibe Mode images and manifest verified"
        if line.startswith("Serial port "):
            return f"flash: {line}"
        if line.startswith("Connecting"):
            return "flash: connecting to ROM Download Mode"
        if line.startswith("Chip is "):
            return f"flash: {line}"
        if "will be erased" in lower or line.startswith("Erasing flash"):
            return f"flash: {line}"
        if line.startswith("Wrote "):
            return f"flash: {line}"
        if "hash of data verified" in lower:
            return "flash: image hash verified"
        if line.startswith("Leaving"):
            return "flash: transfer complete"
        if "hard resetting" in lower:
            return "flash: resetting device into Vibe Mode"
        if "project build complete" in lower:
            return "idf: build complete"
        return None

    return parse


_IDF_ERROR_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "configdep",
        re.compile(r"^.*\bin touch_file: cannot (?:create|close)\b.*$", re.MULTILINE),
    ),
    (
        "compiler",
        re.compile(
            r"^(?:[^:\n]+:)+\d+(?::\d+)?:\s+(?:fatal\s+)?error:\s+.+$",
            re.IGNORECASE | re.MULTILINE,
        ),
    ),
    (
        "linker",
        re.compile(
            r"(?:undefined reference to|will not fit in region|region [`'\"]?.+[`'\"]? "
            r"overflowed|collect2:\s*error|ld(?:\.exe)?:\s*error)",
            re.IGNORECASE,
        ),
    ),
    (
        "partition",
        re.compile(
            r"(?:does not fit|too large for|exceeds .*partition|partition .* overflow|"
            r"app partition is too small)",
            re.IGNORECASE,
        ),
    ),
    (
        "cmake",
        re.compile(
            r"^(?:CMake Error|-- Configuring incomplete, errors occurred)",
            re.IGNORECASE | re.MULTILINE,
        ),
    ),
    ("python", re.compile(r"^Traceback \(most recent call last\):", re.MULTILINE)),
    (
        "generic",
        re.compile(
            r"^(?!ninja: build stopped)(?!FAILED:).*\b(?:fatal|error):\s+.+$",
            re.IGNORECASE | re.MULTILINE,
        ),
    ),
)
_IDF_DIAGNOSTIC_CONTEXT_LINES = 20


def _idf_failure_diagnostic(output: str) -> str | None:
    """Return a bounded, useful root-cause excerpt from raw idf.py output."""

    ansi = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
    clean = ansi.sub("", output).replace("\r\n", "\n").replace("\r", "\n")
    candidates: list[tuple[int, int, str]] = []
    for priority, (category, pattern) in enumerate(_IDF_ERROR_PATTERNS):
        match = pattern.search(clean)
        if match:
            candidates.append((match.start(), priority, category))
    if not candidates:
        failed = re.search(r"^FAILED:.*$", clean, re.MULTILINE)
        if failed is None:
            return None
        candidates.append((failed.start(), len(_IDF_ERROR_PATTERNS), "ninja"))

    offset, _, category = min(candidates, key=lambda item: (item[0], item[1]))
    lines = clean.splitlines()
    line_number = clean.count("\n", 0, offset)
    excerpt = lines[line_number : line_number + _IDF_DIAGNOSTIC_CONTEXT_LINES]
    while excerpt and not excerpt[-1].strip():
        excerpt.pop()
    return f"Build diagnostic ({category}):\n" + "\n".join(excerpt)


def idf_target_command(
    context: RunContext,
    *,
    idf_path: Path,
    project: Path,
    build_dir: Path,
    target: str,
    definitions: dict[str, str] | None = None,
    port: str | None = None,
    timeout: float,
) -> dict[str, Any]:
    try:
        prepared = prepare_idf_environment(idf_path)
    except HostEnvironmentError as error:
        raise BuildError(
            "The ESP-IDF environment could not be prepared.",
            details={"diagnostic": str(error), "log": str(context.log_path)},
        ) from error
    command: list[str | Path] = [
        prepared.python,
        prepared.idf_py,
        "-C",
        project,
        "-B",
        build_dir,
    ]
    for key, value in (definitions or {}).items():
        command.extend(["-D", f"{key}={value}"])
    command.append(target)
    process_environment = configdep_environment(
        context.workspace.configdep, environment=prepared.values
    )
    if port:
        # Custom ESP-IDF flash targets consume ESPPORT in run_serial_tool.cmake;
        # idf.py's -p option is only propagated to its built-in flash actions.
        process_environment["ESPPORT"] = port
    return {"argv": [str(part) for part in command], "cwd": str(context.workspace.root),
            "env": process_environment}


def run_idf_target(
    context: RunContext,
    *,
    idf_path: Path,
    project: Path,
    build_dir: Path,
    target: str,
    definitions: dict[str, str] | None = None,
    port: str | None = None,
    timeout: float,
) -> None:
    command = idf_target_command(context, idf_path=idf_path, project=project,
                                 build_dir=build_dir, target=target, definitions=definitions,
                                 port=port, timeout=timeout)
    try:
        result = context.run(
            command["argv"],
            timeout=timeout,
            cwd=Path(command["cwd"]),
            env=command["env"],
            output_status=_idf_progress_parser(),
        )
    except subprocess.TimeoutExpired as error:
        raise BuildError(
            f"ESP-IDF operation timed out after {timeout:g} seconds.",
            details={"log": str(context.log_path)},
        ) from error
    if result.returncode:
        output = (result.stdout or "").lower()
        if "port is busy" in output or "could not exclusively lock port" in output:
            raise DeviceError(
                "The device configuration channel is busy. Close the process using "
                "the device and try again.",
                details={"log": str(context.log_path)},
            )
        if "could not connect to an espressif device" in output:
            raise DeviceError(
                "Could not connect to the ESP-Mosaico device in recovery "
                "configuration mode.",
                details={"log": str(context.log_path)},
            )
        details = {"log": str(context.log_path)}
        diagnostic = _idf_failure_diagnostic(result.stdout or "")
        if diagnostic:
            details["diagnostic"] = diagnostic
        raise BuildError(f"ESP-IDF target {target} failed.", details=details)

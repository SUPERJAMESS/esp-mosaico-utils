"""Initialize an isolated Git repository without inheriting caller overrides."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess

from .errors import EnvironmentError, OperationError
from .template import shell_quote


def git_executable() -> str:
    executable = shutil.which("git")
    if executable is None:
        raise EnvironmentError("Creating a project requires Git 2.28 or newer on PATH.")
    return executable


def initialize_git(project: Path, executable: str) -> None:
    # Never inherit a caller's GIT_DIR, GIT_WORK_TREE, index or config overrides.
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
    try:
        if (project / ".git").exists() or (project / ".git").is_symlink():
            raise OSError("the creator supplied unexpected .git metadata")
        result = subprocess.run(
            [executable, "-c", "core.bare=false", "init", "--initial-branch=main", "--template=", str(project)],
            cwd=project,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if result.returncode or not (project / ".git").is_dir():
            raise OSError(result.stderr.strip() or "Git did not create a local .git directory")
    except (OSError, subprocess.SubprocessError) as error:
        recovery = "git -C " + shell_quote(str(project)) + " init --initial-branch=main"
        raise OperationError(
            f"Project files were retained at {project}, but Git initialization failed: {error}. "
            f"After fixing the Git environment, run: {recovery}",
            details={"project": str(project), "git_initialized": False, "recovery_command": recovery},
        ) from error

"""Validate and exclusively create projects from workspace-owned descriptions."""

from __future__ import annotations

from pathlib import Path
import re
from typing import Any

from .errors import EnvironmentError, OperationError, SelectionError
from .project_defaults import management_files, merge_management_files
from .project_git import git_executable, initialize_git
from .project_setup import setup_result
from .template import RESERVED_NAMES, render_template, shell_quote
from .workspace import WorkspaceConfig


def _inside(path: Path, parent: Path) -> bool:
    return parent in path.parents


def _make_parents(directory: Path, boundary: Path, created: list[Path]) -> None:
    missing = []
    parent = directory
    while parent != boundary and not parent.exists():
        missing.append(parent)
        parent = parent.parent
    for path in reversed(missing):
        try:
            path.mkdir()
        except FileExistsError:
            if not path.is_dir():
                raise
        else:
            created.append(path)


def _write_project(project: Path, root: Path, files: dict[str, bytes], *, reserve: bool = True) -> None:
    created_dirs = []
    created_files = []
    try:
        if reserve:
            _make_parents(project.parent, root, created_dirs)
            try:
                project.mkdir()  # Exclusive reservation; a concurrent init must fail.
            except FileExistsError as error:
                raise SelectionError(f"Project destination already exists: {project}") from error
            created_dirs.append(project)
        for filename, content in files.items():
            destination = project / filename
            if destination.parent != project and not _inside(destination.parent.resolve(), project):
                raise OSError(f"Project directory escapes the destination: {destination.parent}")
            _make_parents(destination.parent, project, created_dirs)
            with destination.open("xb") as stream:
                created_files.append(destination)
                stream.write(content)
    except (OSError, SelectionError, KeyboardInterrupt) as error:
        if isinstance(error, SelectionError):
            # Another creator owns the project and now needs its parent dirs.
            raise
        cleanup_errors = []
        for path in reversed(created_files):
            try:
                path.unlink()
            except OSError as cleanup_error:
                cleanup_errors.append(f"{path}: {cleanup_error}")
        for path in reversed(created_dirs):
            try:
                path.rmdir()  # Never recursively remove content we did not create.
            except OSError as cleanup_error:
                cleanup_errors.append(f"{path}: {cleanup_error}")
        raise OperationError(
            f"Could not create project {project}: {error or 'interrupted'}",
            details={"project": str(project), "cleanup_errors": cleanup_errors},
        ) from error


def project_destination(workspace: WorkspaceConfig, name: str) -> Path:
    """Apply the same name and destination constraints to every creator."""
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name) or name.lower() in RESERVED_NAMES:
        raise SelectionError(
            "Project name must start with a letter and contain only ASCII letters, "
            "digits and underscores; Windows reserved names are not allowed."
        )
    # ESP-IDF stores the application name in a 32-byte, NUL-terminated field.
    if len(name) > 31:
        raise SelectionError("Project name must be at most 31 characters.")
    root = workspace.root.resolve()
    projects = workspace.projects_dir.resolve()
    if not _inside(projects, root):
        raise SelectionError("workspace.projects_dir must be a directory inside the workspace.")
    for parent in (projects, *projects.parents):
        if parent.exists():
            if not parent.is_dir():
                raise EnvironmentError(f"Project parent is not a directory: {parent}")
            break
    project = projects / name
    if project.exists() or project.is_symlink():
        raise SelectionError(f"Project destination already exists: {project}")
    return project


def initialize_project(
    workspace: WorkspaceConfig, name: str, *, dry_run: bool = False
) -> dict[str, Any]:
    """Render an application, supplement management files and initialize Git."""
    try:
        root = workspace.root.resolve()
        project = project_destination(workspace, name)
        if workspace.init_template is None:
            raise EnvironmentError(
                "workspace.init_template is not configured; set it to a template description JSON file."
            )
        template = workspace.init_template.resolve()
        recovery = workspace.recovery_project.resolve()
        if template == recovery or _inside(template, recovery):
            raise SelectionError(
                "The retained Vibe Mode firmware cannot be used as an application template."
            )
        if not template.is_file():
            raise EnvironmentError(f"workspace.init_template must name a template description JSON file: {template}")
        files = render_template(workspace, template, project, name)
        files, preserved = merge_management_files(files, management_files(workspace, project))
        git = git_executable()
    except (OSError, ValueError) as error:
        raise EnvironmentError(
            f"Could not read the init template or workspace resources: {error}"
        ) from error

    if not dry_run:
        _write_project(project, root, files)
        initialize_git(project, git)
    return {
        "command": "init",
        "status": "dry_run" if dry_run else "created",
        "name": name,
        "project": str(project),
        "template": str(template),
        "files": list(files),
        **setup_result(preserved, dry_run=dry_run),
        "install_command": "python mosaico.py iris system-update --project " + shell_quote(project.relative_to(root).as_posix()),
    }


def finish_engine_project(workspace: WorkspaceConfig, project: Path, result: dict,
                          common: dict[str, bytes], git: str, *, dry_run: bool) -> dict:
    """Supplement Engine output without replacing its source or configuration."""
    if dry_run:
        names = result.get("files", [])
        if not isinstance(names, list) or any(not isinstance(name, str) for name in names):
            raise EnvironmentError("Engine dry-run files must be a list of relative file paths.")
        existing = dict.fromkeys(names)
    else:
        if project.is_symlink() or not project.is_dir():
            raise OperationError(f"Engine did not create the expected project directory: {project}")
        existing = {}
        directories = {}
        for path in project.rglob("*"):
            if path.name.casefold() == ".git":
                raise EnvironmentError("Engine templates must not supply .git metadata.")
            if path.is_symlink():
                raise EnvironmentError(f"Engine template contains a symlink: {path}")
            if path.is_file():
                existing[path.relative_to(project).as_posix()] = None
            elif path.is_dir():
                name = path.relative_to(project).as_posix()
                directories[name.casefold()] = name
        # Empty directories can also conflict with a common file.
        for name in common:
            if name.casefold() in directories:
                raise EnvironmentError(f"Project management file conflicts with a directory: {name}")
            for parent in Path(name).parents:
                spelling = parent.as_posix()
                existing_spelling = directories.get(spelling.casefold(), spelling)
                if existing_spelling != spelling:
                    raise EnvironmentError(f"Conflicting project management directory: {spelling} and {existing_spelling}")
    merged, preserved = merge_management_files(existing, common)
    if not dry_run:
        _write_project(project, workspace.root.resolve(),
                       {name: content for name, content in common.items() if name not in existing}, reserve=False)
        initialize_git(project, git)
    return {**result, "project": str(project), "files": list(merged),
            **setup_result(preserved, dry_run=dry_run)}

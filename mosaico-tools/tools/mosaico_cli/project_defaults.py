"""Render and merge shared project configuration; never execute project tools."""

from __future__ import annotations

from pathlib import Path

from .errors import EnvironmentError
from .template import _file_path, render_template
from .workspace import WorkspaceConfig


def management_files(workspace: WorkspaceConfig, project: Path) -> dict[str, bytes]:
    description = workspace.tool_root / "templates/common/git_quality/mosaico-template.json"
    return render_template(workspace, description, project, project.name)


def merge_management_files(files: dict, common: dict[str, bytes]) -> tuple[dict, list[str]]:
    """Preserve exact matches; reject portable path and file/directory conflicts."""
    merged = dict(files)
    preserved = []
    keys = {}
    spellings = {}
    for name in [*files, *common]:
        path = _file_path(name, "project file")
        if any(part.casefold() == ".git" for part in path.parts):
            raise EnvironmentError("Templates must not supply .git metadata.")
        for length in range(1, len(path.parts) + 1):
            prefix = "/".join(path.parts[:length])
            prior = spellings.setdefault(prefix.casefold(), prefix)
            if prior != prefix:
                raise EnvironmentError(f"Conflicting project management path: {prefix} and {prior}")
        key = name.casefold()
        for other, original in keys.items():
            if key == other and original != name or key.startswith(other + "/") or other.startswith(key + "/"):
                raise EnvironmentError(f"Conflicting project management path: {name} and {original}")
        keys[key] = name
    for name, content in common.items():
        if name in merged:
            preserved.append(name)
        else:
            merged[name] = content
    return merged, preserved

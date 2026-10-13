"""Render and merge shared project configuration; never execute project tools."""

from __future__ import annotations

from pathlib import Path

from .errors import EnvironmentError
from .template import _file_path, render_template
from .workspace import WorkspaceConfig


def management_files(workspace: WorkspaceConfig, project: Path) -> dict[str, bytes]:
    description = workspace.tool_root / "templates/common/git_quality/mosaico-template.json"
    return render_template(workspace, description, project, project.name)


def _preserve_formatter_rules(files: dict, merged: dict, preserved: list[str]) -> None:
    """Adapt only our default hooks; let template-owned hooks choose their rules.

    Use file names alone so Engine dry-runs and real creation agree without
    parsing or executing template configuration. Ruff owns TOML discovery,
    including pyproject.toml files with no Ruff section and nested overrides.
    """
    hook = ".pre-commit-config.yaml"
    if hook in files or hook not in merged:
        return
    config = merged[hook].decode("utf-8")
    astyle = "tools/quality/astyle.yml"
    if astyle not in files and ".astyle-rules.yml" in files:
        config = config.replace("--rules=" + astyle, "--rules=.astyle-rules.yml")
        merged.pop(astyle, None)
        preserved.append(".astyle-rules.yml")
    ruff = "tools/quality/ruff.toml"
    root_rules = [name for name in (".ruff.toml", "ruff.toml", "pyproject.toml") if name in files]
    if ruff not in files and root_rules:
        # Native discovery preserves both root and per-directory settings. An
        # explicit --config would bypass the template's nested configurations.
        config = config.replace("        args: [--config, " + ruff + "]\n", "")
        merged.pop(ruff, None)
        preserved.extend(root_rules)
    merged[hook] = config.encode("utf-8")


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
    _preserve_formatter_rules(files, merged, preserved)
    return merged, preserved

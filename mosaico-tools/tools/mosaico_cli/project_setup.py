"""Report Git initialization and quality setup for created applications."""

from __future__ import annotations


QUALITY_GUIDE = "tools/quality/README.md"


def setup_result(preserved: list[str], *, dry_run: bool) -> dict:
    return {
        "git": {"status": "planned" if dry_run else "initialized", "branch": "main"},
        "quality": {
            "guide": QUALITY_GUIDE,
            "preserved_files": preserved,
            "install_command": "python -m pip install -r tools/quality/requirements.txt",
            "enable_command": "python -m pre_commit install",
        },
    }


def print_setup(result: dict, *, show_files: bool = False) -> None:
    print(f"Git: {result['git']['status']} (independent repository, branch main)")
    if show_files:
        print("Planned project files (including shared management configuration):")
        for name in result["files"]:
            print("  " + name)
    print("From the project directory:")
    print(result["quality"]["install_command"])
    print(result["quality"]["enable_command"])
    print("Quality guide: " + result["quality"]["guide"])
    preserved = result["quality"]["preserved_files"]
    if preserved:
        print("Preserved template configuration (its checks take precedence): " + ", ".join(preserved))

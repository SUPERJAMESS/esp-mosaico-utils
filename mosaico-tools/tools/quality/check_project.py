"""Shared ESP-Mosaico checks. Run from the application root, or pass --project PATH."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
EXTERNAL = {"managed_components", "third_party", "third-party", "vendor", "submodule", "submodules", "generated"}
# IDF Component Manager manifest/constants.py, SLUG_BODY_REGEX (max 64).
COMPONENT_NAME = re.compile(r"[a-zA-Z\d]+(?:(?:[_-](?![_-]+))|(?:[a-zA-Z\d]))*[a-zA-Z\d]+", re.ASCII)
BUILD_DIRECTORY = re.compile(r"(?:build|build[-_].*|cmake-build-.*|gsp_gen_.*)")
# Comments and string literals are ignored when checking configured API calls.
C_NON_CODE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)


def tracked_files(root):
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=root,
        capture_output=True,
        check=True,
        env={key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")},
    )
    return [os.fsdecode(name) for name in result.stdout.split(b"\0") if name]


def artifact_reason(name):
    path = Path(name)
    if any(BUILD_DIRECTORY.fullmatch(part) for part in path.parts[:-1]):
        return "build directory"
    if any(
        part in {"managed_components", "CMakeFiles", "__pycache__", ".codex-runs", ".cache"}
        or part.startswith(".venv")
        or part in {"venv", ".pytest_cache", ".ruff_cache", ".mypy_cache"}
        for part in path.parts[:-1]
    ):
        return "downloaded dependency or local cache"
    if path.name in {
        "sdkconfig",
        "sdkconfig.old",
        "sdkconfig.backup",
        "sdkconfig.local.defaults",
        "CMakeCache.txt",
        "compile_commands.json",
        ".ninja_deps",
        ".ninja_log",
        "ui_apps.bin",
    }:
        return "local configuration or build output"
    if path.suffix.lower() in {".o", ".obj", ".elf", ".map", ".irisfw", ".pyc", ".pyo"}:
        return "compiled output"
    if (
        path.name == ".env"
        or path.name.startswith(".env.")
        and path.name != ".env.example"
        or path.suffix.lower() in {".key", ".pem"}
    ):
        return "local credentials (use documented placeholders instead)"
    return None


def own_source(root, name):
    path = Path(name)
    return (
        path.suffix.lower() in SOURCE_SUFFIXES
        and not EXTERNAL.intersection(path.parts)
        and not (len(path.parts) > 1 and path.parts[0] == "shared" and path.parts[1] in {"common_components", "boards"})
        and not artifact_reason(name)
        and not (root / path).is_symlink()
    )


def artifacts(names):
    errors = []
    for name in names:
        reason = artifact_reason(name)
        if reason:
            errors.append(f"{name}: do not commit {reason}")
    return errors


def load_policy(root):
    policy = json.loads((root / "tools/quality/policy.json").read_text(encoding="utf-8"))
    if not isinstance(policy, dict) or set(policy) - {"forbidden_apis", "generated_checks", "compile_commands"}:
        raise ValueError("policy.json must contain only forbidden_apis, generated_checks and compile_commands")
    apis = policy.get("forbidden_apis", [])
    if not isinstance(apis, list) or any(
        not isinstance(api, str) or not re.fullmatch(r"[A-Za-z_]\w*", api) for api in apis
    ):
        raise ValueError("forbidden_apis must be a list of C identifiers")
    checks = policy.get("generated_checks", [])
    if not isinstance(checks, list) or any(
        not isinstance(command, list) or not command or any(not isinstance(arg, str) or not arg for arg in command)
        for command in checks
    ):
        raise ValueError("generated_checks must be a list of non-empty command argument lists")
    database = policy.get("compile_commands", "build/compile_commands.json")
    if not isinstance(database, str) or not database:
        raise ValueError("compile_commands must be a non-empty path")
    return policy


def components(root, policy):
    names = tracked_files(root)
    errors = []
    component_names = {
        Path(name).parts[1]
        for name in names
        if len(Path(name).parts) > 2 and Path(name).parts[0] == "components" and not Path(name).parts[1].startswith(".")
    }
    for name in sorted(component_names):
        if len(name) > 64 or not COMPONENT_NAME.fullmatch(name):
            errors.append(
                f"components/{name}: use an IDF component name (2-64 ASCII letters/digits with single _ or - separators)"
            )
        if f"components/{name}/CMakeLists.txt" not in names:
            errors.append(f"components/{name}: stage its CMakeLists.txt so ESP-IDF can discover the component")
    apis = policy.get("forbidden_apis", [])
    if apis:
        pattern = re.compile(r"\b(?:" + "|".join(re.escape(api) for api in apis) + r")\s*\(")
        for name in names:
            if own_source(root, name):
                source = (root / name).read_text(encoding="utf-8")
                code = C_NON_CODE.sub(lambda match: "\n" * match[0].count("\n") + " ", source)
                for match in pattern.finditer(code):
                    line = code.count("\n", 0, match.start()) + 1
                    errors.append(f"{name}:{line}: configured forbidden API: {match[0].split('(')[0].strip()}")
    return errors


def cppcheck(root, policy, override):
    executable = shutil.which("cppcheck")
    if not executable:
        raise ValueError("Cppcheck is missing; use the mosaico-cppcheck pre-commit hook or install cppcheck==1.5.1")
    database = Path(override or policy.get("compile_commands", "build/compile_commands.json"))
    if not database.is_absolute():
        database = root / database
    if not database.is_file():
        raise ValueError(
            f"Compilation database missing: {database}. Build the application through the workspace mosaico.py launcher first; "
            "set compile_commands in policy.json or pass --compile-commands PATH for a different build directory."
        )
    entries = json.loads(database.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise ValueError("Compilation database must be a JSON array")
    selected = []
    for entry in entries:
        directory = Path(entry["directory"])
        if not directory.is_absolute():
            directory = database.parent / directory
        source = (directory / entry["file"]).resolve()
        try:
            relative = source.relative_to(root).as_posix()
        except ValueError:
            continue
        if own_source(root, relative):
            selected.append({**entry, "directory": str(directory.resolve()), "file": str(source)})
    if not selected:
        raise ValueError("Compilation database contains no application-owned C/C++ sources")
    # Close before invoking Cppcheck, including on Windows. Never alter the build's database.
    with tempfile.TemporaryDirectory(prefix="mosaico-cppcheck-") as temporary:
        filtered = Path(temporary) / "compile_commands.json"
        filtered.write_text(json.dumps(selected), encoding="utf-8")
        command = [
            executable,
            "--project=" + str(filtered),
            "--enable=warning,style,performance,portability",
            "--error-exitcode=1",
            "--inline-suppr",
            "--suppress=missingIncludeSystem",
        ]
        # Filtering translation units alone still diagnoses their included headers.
        # Exclude vendor/generated headers too, while retaining application errors.
        for directory in sorted(EXTERNAL | {"build", "build-*", "build_*", "cmake-build-*", "gsp_gen_*"}):
            command.append("--suppress=*:*/" + directory + "/*")
        command.extend(["--suppress=*:*/shared/common_components/*", "--suppress=*:*/shared/boards/*"])
        database_directory = database.parent.resolve()
        if database_directory != root and database_directory not in root.parents:
            command.append("--suppress=*:" + database_directory.as_posix() + "/*")
        result = subprocess.run(command, cwd=root, check=False)
    return [] if result.returncode == 0 else ["Cppcheck found problems; see diagnostics above"]


def generated(root, policy):
    checks = policy.get("generated_checks", [])
    if not checks:
        print("No generated-source checks configured; build outputs stay untracked.")
    for command in checks:
        # {python} avoids depending on a shell's python/python3 alias.
        command = [sys.executable if arg == "{python}" else arg for arg in command]
        result = subprocess.run(command, cwd=root, check=False)
        if result.returncode:
            return [f"Generated-source check failed ({result.returncode}): {command}"]
    return []


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("check", choices=("artifacts", "components", "cppcheck", "generated"))
    parser.add_argument(
        "--project", type=Path, default=Path.cwd(), help="Application root (default: current directory)"
    )
    parser.add_argument("--compile-commands", help="Compilation database override for cppcheck")
    parser.add_argument("files", nargs="*")
    arguments = parser.parse_args(argv)
    root = arguments.project.resolve()
    try:
        if not (root / ".git").exists():
            raise ValueError("Run checks from the application Git root or pass --project PATH")
        if arguments.check == "artifacts":
            errors = artifacts(arguments.files or tracked_files(root))
        else:
            policy = load_policy(root)
            if arguments.check == "components":
                errors = components(root, policy)
            elif arguments.check == "cppcheck":
                errors = cppcheck(root, policy, arguments.compile_commands)
            else:
                errors = generated(root, policy)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        errors = [str(error)]
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

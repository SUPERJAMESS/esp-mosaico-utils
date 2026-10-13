"""Workspace application creation and preview using public dependency entry points."""
from __future__ import annotations

from dataclasses import replace
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys

from .errors import EnvironmentError, SelectionError
from .project import discover_artifacts, resolve_project
from .scaffold import finish_engine_project, initialize_project, project_destination
from .project_defaults import management_files
from .project_git import git_executable
from .project_setup import print_setup

GAME_TEMPLATES = {"shooter": "raylib_shooter", "sky-hop": "sky_hop", "tower-defense": "tower_defense"}
DEFAULT_GAME_TEMPLATE = "blank"

IRIS_PROJECT_CMAKE = """cmake_minimum_required(VERSION 3.16)
set(RAYLIB_LITE_GAME {game})
set(RAYLIB_LITE_GAME_DIR {source})
include("{module}")
project({game} VERSION {version})
raylib_lite_iris_link_game_board()
include("${{MOSAICO_SYSTEM_UPDATE_CMAKE}}")
"""


def _engine(workspace, purpose):
    engine = workspace.raylib_path
    if engine is None or not (engine / "tools/game_cli.py").is_file():
        raise EnvironmentError(f"Initialize the configured Raylib Lite Engine dependency before {purpose}.")
    return engine


def _engine_games(engine, target):
    completed = subprocess.run(
        [sys.executable, str(engine / "tools/game_cli.py"), "list", "--json"],
        capture_output=True, text=True,
    )
    if completed.returncode:
        raise EnvironmentError(completed.stderr.strip() or "Engine game listing failed.")
    games = json.loads(completed.stdout)["games"]
    return {game["name"]: Path(game["path"]) for game in games
            if not game["name"].endswith("_dev")
            and (game.get("host", False) if target == "host" else "esp-mosaico" in game.get("boards", []))}


def _cmake_literal(value):
    """Quote paths without interpreting CMake variables or escape sequences."""
    marker = "="
    while "]" + marker + "]" in value:
        marker += "="
    return "[" + marker + "[" + value + "]" + marker + "]"


def _iris_project(workspace, selected):
    """Wrap a user-owned native game or a named Engine example."""
    engine = _engine(workspace, "an Iris build")
    requested = Path(selected).expanduser()
    explicit = requested.exists() or requested.is_absolute() or len(requested.parts) > 1
    if explicit:
        source = resolve_project(workspace, str(requested), Path.cwd())
    else:
        games = _engine_games(engine, "native")
        if selected not in games:
            raise SelectionError(f"Not a native game: {selected}; specify a project path or choose one of {', '.join(sorted(games))}.")
        source = games[selected].resolve()
    if not (source / "main/CMakeLists.txt").is_file():
        raise SelectionError(f"An Iris game needs a main/CMakeLists.txt component: {source}")
    name = source.name
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", name):
        raise SelectionError("Game directory names must start with a letter or underscore and contain only letters, digits, underscores or hyphens.")
    version_file = source / "version.txt"
    version = version_file.read_text(encoding="utf-8").strip() if version_file.is_file() else "1.0.0"
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,3}", version):
        raise SelectionError(f"version.txt must contain a numeric CMake project version: {version_file}")
    template = workspace.tool_root / "templates" / "blank_game"
    # Source paths, including named examples in another Engine checkout, must
    # never reuse each other's component-manager lock or build cache.
    key = name + "-" + hashlib.sha256(str(source).encode()).hexdigest()[:12]
    project = workspace.run_dir / "raylib-iris" / key / "project"
    project.mkdir(parents=True, exist_ok=True)
    (project / "CMakeLists.txt").write_text(IRIS_PROJECT_CMAKE.format(
        game=name, source=_cmake_literal(source.as_posix()), version=version,
        module=(workspace.tool_root / "cmake/raylib_lite_iris_app.cmake").as_posix()), encoding="utf-8")
    (project / "sdkconfig.game.defaults").write_text(
        'CONFIG_APP_PROJECT_VER="' + version + '"\n', encoding="utf-8")
    (project / "partitions.csv").write_bytes((template / "partitions.csv").read_bytes())
    return project


def add_commands(commands, project_commands):
    preview = project_commands.add_parser("sim", help="Preview a GSP application with its native PC backend")
    preview.set_defaults(command="sim", public_command="project sim")
    preview.add_argument("--project")
    preview.add_argument("--scene", help="Preview one scene without the PC backend; path relative to the project or absolute")
    preview.add_argument("--scene-only", action="store_true")
    mode = preview.add_mutually_exclusive_group()
    mode.add_argument("--headless", action="store_true")
    mode.add_argument("--interactive", action="store_true")
    preview.add_argument("--duration", type=float)
    preview.add_argument("--frames", type=int)
    preview.add_argument("--fps", type=int)
    preview.add_argument("--dump-ppm")
    game = commands.add_parser("game", help="Create blank games or Engine examples and use the Raylib Host simulator")
    actions = game.add_subparsers(dest="game_action", required=True)
    for name in ("create", "new"):
        create = actions.add_parser(name, help="Create a blank game or a complete Engine example")
        create.set_defaults(command="game", public_command="game " + name)
        create.add_argument("name")
        create.add_argument("--template", choices=(DEFAULT_GAME_TEMPLATE, *GAME_TEMPLATES),
                            default=DEFAULT_GAME_TEMPLATE, help="Game template (default: blank)")
        create.add_argument("--dry-run", action="store_true")
    for name in ("sim", "run", "build"):
        child = actions.add_parser(name)
        child.set_defaults(command="game", public_command="game " + name)
        child.add_argument("project_path", nargs="?")
        child.add_argument("--project")
        if name == "build":
            child.add_argument("--idf-path")
            child.add_argument("--target", choices=("native", "iris"), default="native",
                               help="native: build the project as is; iris: build a native game project path or named engine example as an ESP-Iris app")
        else:
            child.add_argument("--headless", action="store_true")
            child.add_argument("--frames", type=int, default=300)
            child.add_argument("--listen", choices=("127.0.0.1", "0.0.0.0"), default="127.0.0.1")
            child.add_argument("--port", type=int, default=8460)
            child.add_argument("--scenario", "--replay", dest="replay")
            child.add_argument("--state-output")


def run(arguments, workspace):
    tools_root = workspace.recovery_project.parents[2] / "mosaico-tools"
    if arguments.command == "sim":
        project = resolve_project(workspace, arguments.project, Path.cwd())
        command = [sys.executable, str(tools_root / "tools/gsp-sim/run.py"),
                   "--project", str(project)]
        for flag in ("headless", "interactive", "scene_only"):
            if getattr(arguments, flag):
                command.append("--" + flag.replace("_", "-"))
        for flag in ("duration", "frames", "fps", "dump_ppm"):
            value = getattr(arguments, flag)
            if value is not None:
                command.extend(("--" + flag.replace("_", "-"), str(value)))
        if arguments.scene is not None:
            command.append(arguments.scene)
        return subprocess.call(command)

    if arguments.game_action in {"create", "new"}:
        if arguments.template != DEFAULT_GAME_TEMPLATE:
            engine = _engine(workspace, "creating a game")
            destination = project_destination(workspace, arguments.name)
            common = management_files(workspace, destination)
            git = git_executable()
            command = [sys.executable, str(engine / "tools/game_cli.py"), "create", str(destination),
                       "--template", arguments.template, "--json"]
            if arguments.dry_run:
                command.append("--dry-run")
            completed = subprocess.run(command, capture_output=True, text=True)
            if completed.returncode:
                raise SelectionError(completed.stderr.strip() or "Engine game creation failed.")
            result = json.loads(completed.stdout)
            result = finish_engine_project(workspace, destination, result, common, git, dry_run=arguments.dry_run)
            result["status"] = "dry_run" if arguments.dry_run else "created"
            result["install_command"] = "python mosaico.py game build " + str(destination) + " --target iris"
            print(json.dumps(result) if arguments.json else f"game: {result['status']}\nProject: {result['project']}\n{result['install_command']}")
            if not arguments.json:
                print_setup(result, show_files=arguments.dry_run)
            return 0
        template = tools_root / "templates/blank_game/mosaico-template.json"
        if not template.is_file():
            owner = "utils" if arguments.template == DEFAULT_GAME_TEMPLATE else "BSP"
            raise EnvironmentError(f"Initialize the {owner} submodule containing the selected game template: " + str(template))
        engine = workspace.raylib_path
        if engine is None or not all((engine / name).is_file() for name in ("idf_component.yml", "CMakeLists.txt")):
            raise EnvironmentError("Initialize the configured Raylib Lite Engine dependency before creating a game.")
        result = initialize_project(replace(workspace, init_template=template), arguments.name, dry_run=arguments.dry_run)
        if arguments.json:
            print(json.dumps({"ok": True, **result}, ensure_ascii=False, sort_keys=True))
        else:
            print(f"game: {result['status']}\nProject: {result['project']}\n{result['install_command']}")
            print_setup(result, show_files=arguments.dry_run)
        return 0

    if arguments.project and arguments.project_path:
        raise SelectionError("Use either the positional project or --project.")
    iris = arguments.game_action == "build" and arguments.target == "iris"
    selected = arguments.project or arguments.project_path
    if iris:
        if not selected:
            raise SelectionError("Specify an engine game for an Iris build.")
        project = _iris_project(workspace, selected)
    else:
        candidate = workspace.raylib_path / selected if workspace.raylib_path and selected else None
        required = "CMakeLists.txt" if arguments.game_action == "build" else "game.sim.json"
        project = (candidate.resolve() if candidate and not Path(selected).exists() and (candidate / required).is_file()
                   else resolve_project(workspace, selected, Path.cwd()))
    if arguments.game_action == "build":
        import os
        from .runtime import RunContext, build_application
        if arguments.idf_path:
            os.environ["IDF_PATH"] = str(Path(arguments.idf_path).expanduser().resolve())
        os.environ["RAYLIB_LITE_ENGINE_ROOT"] = str(workspace.raylib_path)
        os.environ["MOSAICO_UTILS_ROOT"] = str(workspace.tool_root.parent)
        os.environ["MOSAICO_BSP_ROOT"] = str(workspace.bsp_path)
        os.environ["RAYLIB_LITE_BSP_DIR"] = str(workspace.bsp_path)
        os.environ["RAYLIB_LITE_UTILS_DIR"] = str(workspace.tool_root.parent)
        context = RunContext(workspace, "game-build", arguments.verbose, arguments.json)
        build_application(context, project)
        if iris:
            from .gateway import ensure_iris_tools
            from .runtime import resolve_idf_path, run_idf_target
            iris_python, _ = ensure_iris_tools(context)
            run_idf_target(context, idf_path=resolve_idf_path(workspace, project),
                           project=project, build_dir=project / "build",
                           target="system-update-bundle",
                           definitions={"ESP_IRIS_PYTHON": str(iris_python)}, timeout=900)
            context.status(f"system update bundle: {project / 'build' / (discover_artifacts(project).project_name + '-system-update.irisfw')}")
        return 0
    engine = workspace.raylib_path
    if engine is None or not (engine / "host/run_game.py").is_file():
        raise EnvironmentError("Initialize the configured Raylib Lite Engine dependency before simulation.")
    command = [sys.executable, str(engine / "host/run_game.py"), "--project", str(project),
               "--listen", arguments.listen, "--port", str(arguments.port)]
    if arguments.headless:
        command.extend(("--headless", "--frames", str(arguments.frames)))
    for name in ("replay", "state_output"):
        value = getattr(arguments, name)
        if value:
            command.extend(("--" + name.replace("_", "-"), value))
    return subprocess.call(command)

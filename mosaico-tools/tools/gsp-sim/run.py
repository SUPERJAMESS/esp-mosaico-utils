#!/usr/bin/env python3
"""Run a GSP app preview using its component Host adapter or legacy pc/ backend."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent

sys.path.insert(0, str(TOOLS_DIR))
from fetch_gspc import resolve_gsp_root, resolve_gspc, resolve_sim  # noqa: E402


def project_scene(project: Path) -> Path:
    """Select a unique authored entry scene; never guess between components."""
    candidates = sorted(path for path in project.glob("components/*/ui/main.json") if path.is_file())
    legacy = project / "ui/main.json"
    if legacy.is_file():
        candidates.insert(0, legacy)
    if not candidates:
        raise SystemExit(f"No scene found in {project}: expected ui/main.json or components/*/ui/main.json")
    if len(candidates) > 1:
        names = ", ".join(path.relative_to(project).as_posix() for path in candidates)
        raise SystemExit(f"Multiple scenes in {project}: {names}. Use project sim --scene PATH or pass a scene path to this script.")
    return candidates[0]


def scene_project(scene: Path) -> Path:
    """Locate the owning app, regardless of component depth, for dependency lookup.

    A component CMakeLists.txt is not an application marker. For standalone
    scenes without an app, use their directory and the explicit tool overrides.
    """
    for parent in scene.parents:
        if ((parent / "pc/CMakeLists.txt").is_file()
                or (parent / "sdkconfig.defaults").is_file()
                or (parent / "managed_components").is_dir()
                or (parent / ".git").exists()):
            return parent
    return scene.parent


def project_backend(project: Path, scene: Path) -> Path | None:
    """Use the selected scene's component adapter, then the legacy app backend."""
    host = scene.parent.parent / "host"
    if host.parent.parent == project / "components" and (host / "CMakeLists.txt").is_file():
        return host
    legacy = project / "pc"
    return legacy if (legacy / "CMakeLists.txt").is_file() else None


def backend_project(backend: Path) -> Path:
    """Resolve the app for explicit --pc-project in either supported layout."""
    if backend.name == "host" and backend.parent.parent.name == "components":
        return backend.parents[2]
    return backend.parent


def sim_bridge_script(gsp_root: Path) -> Path:
    return gsp_root / "tools" / "sim_bridge" / "run.py"


def requests_sim_bridge(
    *,
    scene: Path,
    pc: Path | None,
    scene_only: bool,
    dump_ppm: Path | None,
    extra: list[str],
    frames: int | None,
    fps: int | None,
) -> bool:
    """Return whether the CLI arguments select the application backend."""
    return (
        not scene_only
        and pc is not None
        and scene.suffix != ".gspb"
        and dump_ppm is None
        and not extra
        and frames is None
        and fps is None
    )


def run_sim_bridge(pc: Path, gsp_root: Path, *, project: Path, headless: bool,
                   duration: float | None = None) -> int:
    command = [
        sys.executable,
        str(sim_bridge_script(gsp_root)),
        "--project",
        str(pc),
        "--component-dir",
        str(gsp_root),
        "--gspc",
        str(resolve_gspc(gsp_root=gsp_root)),
        "--host",
        str(resolve_sim(gsp_root=gsp_root)),
        "--build-dir",
        str(project / "build-sim"),
    ]
    if headless:
        command.append("--headless")
    if duration is not None:
        command.extend(("--duration", str(duration)))
    return subprocess.call(command)


def pack_scene(scene: Path, output: Path, gspc: Path) -> None:
    command = [
        str(gspc),
        "pack",
        str(scene),
        "--pixel-format",
        "rgb565",
        "--deployable",
        "-o",
        str(output),
    ]
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "scene",
        nargs="?",
        type=Path,
        help="scene-only preview of JSON or .gspb (relative to --project when provided)",
    )
    parser.add_argument("--project", type=Path,
                        help="Application directory; selects ui/main.json or a unique components/*/ui/main.json")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--interactive", action="store_true")
    parser.add_argument(
        "--scene-only",
        action="store_true",
        help="Skip sim_bridge and preview the packed scene without a native backend",
    )
    parser.add_argument("--frames", type=int)
    parser.add_argument("--fps", type=int)
    parser.add_argument("--duration", type=float,
                        help="Stop a sim_bridge preview after this many seconds")
    parser.add_argument(
        "--pc-project",
        type=Path,
        help="run a Host backend CMake directory directly (component host/ or legacy pc/)",
    )
    parser.add_argument("--dump-ppm", type=Path)
    parser.add_argument(
        "sim_args",
        nargs=argparse.REMAINDER,
        help="extra simulator flags; pass after -- (for example -- --drag 80 360 400 360)",
    )
    args = parser.parse_args()
    if args.headless and args.interactive:
        raise SystemExit("use either --headless or --interactive")

    if args.pc_project is not None:
        pc = args.pc_project.expanduser().resolve()
        if not (pc / "CMakeLists.txt").is_file():
            raise SystemExit(f"PC simulator project not found: {pc}")
        project = backend_project(pc)
        gsp_root = resolve_gsp_root(project)
        if gsp_root is None or not sim_bridge_script(gsp_root).is_file():
            raise SystemExit(
                "espressif/esp-gsp with sim_bridge is not installed for "
                f"{project}"
            )
        bridge_args = {"project": project, "headless": args.headless}
        if args.duration is not None:
            bridge_args["duration"] = args.duration
        return run_sim_bridge(pc, gsp_root, **bridge_args)

    if args.scene is None and args.project is None:
        parser.error("select an application with --project PATH or provide a scene")
    project = args.project.expanduser().resolve() if args.project is not None else None
    if args.scene is not None:
        # Explicit scenes may be used with --project to select one component;
        # relative paths then refer to the application, not the shell's cwd.
        scene = args.scene.expanduser()
        if project is not None and not scene.is_absolute():
            scene = project / scene
        scene = scene.resolve()
    else:
        assert project is not None
        scene = project_scene(project)
    if not scene.is_file():
        raise SystemExit(f"scene not found: {scene}")
    project = project or scene_project(scene)

    extra = list(args.sim_args)
    if extra and extra[0] == "--":
        extra = extra[1:]

    pc = project_backend(project, scene)
    bridge_requested = requests_sim_bridge(
        scene=scene,
        pc=pc,
        # A PC backend compiles its own scene list. An explicit scene must not
        # silently run that backend with a different, CMake-selected scene.
        scene_only=args.scene_only or args.scene is not None,
        dump_ppm=args.dump_ppm,
        extra=extra,
        frames=args.frames,
        fps=args.fps,
    )
    if bridge_requested:
        assert pc is not None
        gsp_root = resolve_gsp_root(project)
        if gsp_root is None:
            raise SystemExit(
                "espressif/esp-gsp is not installed. From the application "
                "directory run `idf.py reconfigure` to pull "
                "espressif/esp-gsp==1.5.1 from the ESP Component Registry, "
                "or set ESP_GSP_COMPONENT_DIR."
            )
        if sim_bridge_script(gsp_root).is_file():
            if args.duration is None:
                return run_sim_bridge(pc, gsp_root, project=project, headless=args.headless)
            return run_sim_bridge(pc, gsp_root, project=project, headless=args.headless,
                                  duration=args.duration)

    sim_args: list[str] = []
    if args.headless:
        sim_args.append("--headless")
        if args.frames is None:
            sim_args.extend(["--frames", "30"])
    elif args.frames is None:
        sim_args.extend(["--frames", "0"])
    if args.frames is not None:
        sim_args.extend(["--frames", str(args.frames)])
    sim_args.extend(["--fps", str(args.fps if args.fps is not None else 60)])
    if args.dump_ppm is not None:
        dump = args.dump_ppm.expanduser().resolve()
        dump.parent.mkdir(parents=True, exist_ok=True)
        sim_args.extend(["--dump", str(dump), "--dump-format", "ppm"])
    sim_args.extend(extra)

    gsp_root = resolve_gsp_root(project)
    gspc = resolve_gspc(gsp_root=gsp_root)
    simulator = resolve_sim(gsp_root=gsp_root)
    os.environ["GSPC_EXECUTABLE"] = str(gspc)
    os.environ["GSP_SIM_EXECUTABLE"] = str(simulator)

    with tempfile.TemporaryDirectory(prefix="mosaico-gsp-sim-") as directory:
        if scene.suffix == ".gspb":
            bundle = scene
        else:
            bundle = Path(directory) / "preview.gspb"
            pack_scene(scene, bundle, gspc)
        command = [str(simulator), "--bundle", str(bundle), *sim_args]
        return subprocess.call(command)


if __name__ == "__main__":
    raise SystemExit(main())

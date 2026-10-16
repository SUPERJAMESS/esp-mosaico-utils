"""Author input and deterministic, offline creation of a single Ideas ZIP."""
from __future__ import annotations

import io
from pathlib import Path
import tempfile
import zipfile

from .ideas_content import (ARCHIVE_BYTES, descriptor, encoded, image_check,
                            path_valid, require, strict_json, validate_paths)
from .ideas_release import validate_release

AUTHOR_SCHEMA = "mosaico-ideas/project-release/1"


def source_path(root, name):
    require(isinstance(name, str) and not Path(name).is_absolute() and ".." not in Path(name).parts
            and "\\" not in name, "Source must be a project-relative path")
    path = root / name
    require(all(not parent.is_symlink() for parent in [path, *path.parents] if parent != root.parent)
            and root in path.resolve().parents and path.is_file(), "Missing/linked source file", name)
    return path


def read_source(path, limit=ARCHIVE_BYTES):
    with path.open("rb") as stream:
        value = stream.read(limit+1)
    require(0 < len(value) <= limit, "Source file exceeds byte limit", str(path))
    return value


def generated_resources(config, locale, root):
    require(set(config) <= {"mode", "name", "description", "font", "icon"}
            and {"name", "description", "font"} <= set(config),
            "Generated display requires explicit text/font; do not mix input modes")
    from shared.ideas import generate_resources
    icon = source_path(root, config["icon"]) if config.get("icon") else None
    if icon: image_check(read_source(icon, 10*1024*1024), config["icon"])
    try:
        return generate_resources(name=config["name"], description=config["description"],
                                  locale=locale, font=source_path(root, config["font"]), icon=icon)
    except ImportError as error:
        from .errors import SelectionError
        raise SelectionError("Display generation requires requirements-ideas.txt and the FreeType/libraqm runtime; run mosaico.py with that Python environment.") from error


def package_project(project, output, bundle=None):
    root = Path(project).resolve()
    config = strict_json(read_source(root / "mosaico-ideas.json", 65536))
    import json
    from jsonschema import Draft202012Validator
    schema = json.loads((Path(__file__).resolve().parents[2] / "mosaico-ideas.schema.json").read_text())
    error = next(Draft202012Validator(schema).iter_errors(config), None)
    require(error is None, "Author configuration violates mosaico-ideas.schema.json")
    require(config.get("schema") == AUTHOR_SCHEMA and set(config) <= {"schema", "project", "firmware", "website", "device"}
            and {"schema", "project", "website", "device"} <= set(config), "Invalid author configuration schema")
    files = {}
    firmware = Path(bundle).resolve() if bundle else source_path(root, config.get("firmware"))
    files["firmware/app.irisfw"] = read_source(firmware, 32*1024*1024)
    website = {}
    require(isinstance(config["website"], dict) and set(config["website"]) <= {"readme", "changelog", "covers", "resources", "attachments"},
            "Unknown website configuration field")
    def put(name):
        require(path_valid(name) and name.lower() != "manifest.json" and not name.startswith(("firmware/", "device/")),
                "Invalid website publication path", str(name))
        require(name not in files, "Each file must be declared once", name)
        files[name] = read_source(source_path(root, name))
        return descriptor(name, files[name])
    for group in ("readme", "changelog"):
        if group in config["website"]: website[group] = put(config["website"][group])
    for group in ("covers", "resources", "attachments"):
        names = config["website"].get(group, [])
        require(isinstance(names, list), "Website file group must be an array")
        website[group] = [put(name) for name in names]
    device = config["device"]
    require(isinstance(device, dict) and set(device) == {"default_locale", "presentations"}
            and isinstance(device["presentations"], dict) and set(device["presentations"]) == {"zh-CN", "en"},
            "Explicit Chinese and English presentations are required")
    presentations = []
    for locale in ("zh-CN", "en"):
        source = device["presentations"][locale]
        require(isinstance(source, dict), "Invalid presentation input")
        if source.get("mode", "generate") == "generate":
            resources = generated_resources(source, locale, root)
        else:
            require(source.get("mode") == "prebuilt" and set(source) == {"mode", "display"},
                    "Choose generate or prebuilt input for each locale")
            display_path = source_path(root, source["display"])
            raw = read_source(display_path, 32768)
            display = strict_json(raw, 32768, 8)
            require(set(display) == {"schema", "locale", "icon", "name", "description"}, "Invalid prebuilt display")
            names = {display[field]["font"] for field in ("name", "description")}
            if display["icon"]: names.add(display["icon"])
            require(all(path_valid(name) and "/" not in name and name.lower() != "display.json" for name in names),
                    "Prebuilt resources must use basenames")
            resources = {"display.json": raw}
            for name in names:
                resources[name] = read_source(source_path(root, (display_path.parent / name).relative_to(root).as_posix()), 1024*1024)
        prefix = "device/" + locale + "/"
        for name, data in resources.items(): files[prefix + name] = data
        presentations.append({"display": descriptor(prefix + "display.json", resources["display.json"]),
                              "assets": [descriptor(prefix + name, resources[name]) for name in sorted(resources) if name != "display.json"]})
    manifest = {"schema": "mosaico-ideas/release/1", "project": config["project"],
                "firmware": descriptor("firmware/app.irisfw", files["firmware/app.irisfw"]),
                "website": website, "device": {"profile": "esp-mosaico/v2", "default_locale": device["default_locale"],
                                                "presentations": presentations}}
    files["manifest.json"] = encoded(manifest)
    validate_paths(files)
    require(sum(map(len, files.values())) <= ARCHIVE_BYTES, "Release exceeds 100 MiB")
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=False, compresslevel=9) as archive:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, files[name])
    raw = stream.getvalue()
    result = validate_release(raw)
    target = Path(output).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".ideas-", delete=False) as pending:
        pending.write(raw)
        temporary = Path(pending.name)
    try: temporary.replace(target)
    finally: temporary.unlink(missing_ok=True)
    return {"command": "project package", "package": str(target), "size": len(raw),
            "sha256": result.archive_sha256, "version": result.firmware["release"], "files": len(files)}


def project_package(arguments, context):
    from .project import resolve_project
    project = resolve_project(context.workspace, arguments.project, Path.cwd())
    return package_project(project, arguments.output, arguments.bundle)


def project_validate(arguments, context):
    release = validate_release(Path(arguments.package).expanduser())
    return {"command": "project validate", "status": "valid", "sha256": release.archive_sha256,
            "version": release.firmware["release"], "files": len(release.files)}

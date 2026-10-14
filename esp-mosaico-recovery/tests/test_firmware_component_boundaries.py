"""Enforce the runtime component boundaries independently of an IDF build."""
from pathlib import Path
import re
import shlex


RECOVERY = Path(__file__).resolve().parents[1] / "firmware/recovery"
COMPONENTS = RECOVERY / "components"
OPTIONS = {"SRCS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS", "REQUIRES",
           "PRIV_REQUIRES", "WHOLE_ARCHIVE"}


def registration(component):
    source = (component / "CMakeLists.txt").read_text()
    body = source.split("idf_component_register(", 1)[1].split(")", 1)[0]
    fields = {}
    field = None
    for token in shlex.split(body, comments=True):
        if token in OPTIONS:
            field = token
            fields[field] = []
        elif field:
            fields[field].append(token)
    return fields


def test_application_dependency_graph_is_acyclic_and_never_points_to_main():
    applications = {path.name: registration(path)
                    for path in COMPONENTS.iterdir() if path.is_dir()}
    assert applications
    assert all(name.startswith("app_") for name in applications)
    # This name belongs to the SDK OTA component; shadowing it breaks OTA APIs.
    assert "app_update" not in applications
    graph = {}
    for name, fields in applications.items():
        dependencies = fields.get("REQUIRES", []) + fields.get("PRIV_REQUIRES", [])
        assert "main" not in dependencies, name
        graph[name] = {item for item in dependencies if item in applications}
        assert fields["INCLUDE_DIRS"][0] == "include", name
        private_headers = list((COMPONENTS / name / "private").glob("*.h"))
        assert fields.get("PRIV_INCLUDE_DIRS", []) == (["private"] if private_headers else []), name
        assert (COMPONENTS / name / "README.md").is_file(), name

    def visit(name, active, completed):
        assert name not in active, "cycle: " + " -> ".join(active + [name])
        if name in completed:
            return
        for dependency in graph[name]:
            visit(dependency, active + [name], completed)
        completed.add(name)

    completed = set()
    for name in graph:
        visit(name, [], completed)


def test_private_headers_and_mutable_state_do_not_cross_components():
    private = {path.name: path.parents[1].name
               for path in COMPONENTS.glob("*/private/*.h")}
    public = {path.name: path.parents[1].name
              for path in COMPONENTS.glob("*/include/*.h")}
    for component in COMPONENTS.iterdir():
        fields = registration(component)
        dependencies = set(fields.get("REQUIRES", []) + fields.get("PRIV_REQUIRES", []))
        for path in list(component.rglob("*.c")) + list(component.rglob("*.h")):
            source = path.read_text()
            for include in re.findall(r'^#include "([^"]+)"', source, re.M):
                assert ".." not in Path(include).parts, path
                if include in private:
                    assert private[include] == component.name, (path, include)
                    assert path.parent.name != "include", (path, include)
                if include in public and public[include] != component.name:
                    assert public[include] in dependencies, (path, include)
                    if path.parent.name == "include":
                        assert public[include] in fields.get("REQUIRES", []), (path, include)
            # Function declarations and immutable generated assets are allowed;
            # mutable extern variables are not a business-state interface.
            for declaration in re.findall(r'^extern[ \t]+(?!")([^;{]+);', source, re.M):
                assert "(" in declaration or declaration.startswith("const "), path


def test_entrypoint_and_pc_backend_only_use_public_service_interfaces():
    fields = registration(RECOVERY / "main")
    assert fields["SRCS"] == ["main.c"]
    assert sorted(path.name for path in (RECOVERY / "main").glob("*.[ch]")) == ["main.c"]
    source = (RECOVERY / "main/main.c").read_text()
    assert "gpio_config(" not in source
    assert source.index("factory_system_update_register()") < source.index("recovery_ota_support_start()")
    assert source.index("app_iris_storage_register()") < source.index("recovery_ota_support_start()")
    assert source.index("recovery_ota_support_start()") < source.index("factory_ui_start()")
    assert source.index("app_iris_input_register(") < source.index("esp_iris_mark_healthy()")
    assert "factory_ideas_init(" not in (COMPONENTS / "app_ui/src/factory_ui.c").read_text()
    pc = (RECOVERY / "pc/platform_pc.c").read_text()
    assert "vibe_ui_get_status(" in pc and "vibe_ui_create(" in pc
    assert not re.search(r"\bstate\s*(?:->|\.)", pc)
    assert "struct vibe_ui {" not in (COMPONENTS / "app_ui/include/vibe_ui.h").read_text()

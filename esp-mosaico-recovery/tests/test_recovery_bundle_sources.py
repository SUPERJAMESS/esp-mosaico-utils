"""A candidate must report changes in all of its owned boot/runtime sources."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess

import pytest


@pytest.mark.parametrize("changed", [
    "components/app_bridge/src/iris_bridge.c",
    "../../components/esp_mosaico_boot/mosaico_boot.c",
    "../../include/mosaico_recovery_contract.h",
    "../../product_contract.json",
    "../../cmake/mosaico_idf_project.cmake",
    "../../../ESP-Iris/components/esp_iris/src/esp_iris.c",
])
def test_candidate_source_state_includes_shared_components(tmp_path, monkeypatch, changed):
    tool = Path(__file__).resolve().parents[1] / "firmware/recovery/tools/prepare_recovery.py"
    monkeypatch.syspath_prepend(str(tool.parent))
    spec = importlib.util.spec_from_file_location("candidate_sources", tool)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    source = tmp_path / "esp-mosaico-recovery/firmware/recovery"
    source.mkdir(parents=True)
    component = (source / changed).resolve()
    component.parent.mkdir(parents=True, exist_ok=True)
    component.write_text("original\n")
    unrelated = tmp_path / "unrelated.txt"
    unrelated.write_text("original\n")

    def git(*args):
        return subprocess.run(["git", "-C", str(tmp_path), *args],
                              check=True, capture_output=True, text=True).stdout.strip()

    git("init", "-q")
    git("add", ".")
    git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
        "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
    commit = git("rev-parse", "HEAD")
    assert module.source_state(source) == (commit, False)
    unrelated.write_text("unrelated local edit\n")
    assert module.source_state(source) == (commit, False)
    component.write_text("new boot/runtime implementation\n")
    assert module.source_state(source) == (commit, True)

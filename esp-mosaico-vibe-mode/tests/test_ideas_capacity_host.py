"""Check library admission after a real service restart, before any UI scan."""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from test_ideas_worker_host import worker

__all__ = ["worker"]

pytestmark = pytest.mark.skipif(sys.platform != "linux", reason="requires Linux worker fixture")


def run(worker, scenario, mount):
    executable, fixtures = worker
    result = subprocess.run([str(executable), scenario, str(fixtures), str(mount)],
                            capture_output=True, text=True, timeout=30, check=False)
    assert result.returncode == 0, result.stdout + result.stderr


def contents(mount):
    return {p.relative_to(mount): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in Path(mount).rglob("*") if p.is_file()}


@pytest.fixture(scope="module")
def full_library(worker, tmp_path_factory):
    mount = tmp_path_factory.mktemp("full-library")
    run(worker, "seed-full-library", mount)
    return mount


@pytest.mark.parametrize("kind", ["cloud", "development", "update", "scan-error"])
def test_full_library_after_restart(worker, full_library, tmp_path, kind):
    mount = tmp_path / "nand"
    shutil.copytree(full_library, mount)
    before = contents(mount)
    run(worker, "capacity-" + kind, mount)
    assert len(list((mount / "ideas").iterdir())) == 16
    if kind != "update":
        assert contents(mount) == before

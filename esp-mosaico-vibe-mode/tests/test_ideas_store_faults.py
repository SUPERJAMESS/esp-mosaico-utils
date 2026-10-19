"""Transient resource failures must not delete a healthy saved generation."""
import ctypes as C
import hashlib
import shutil
import sys
from pathlib import Path

import pytest
from test_ideas_store import Package, build_core, callbacks, descriptor, parse, save

pytestmark = pytest.mark.skipif(sys.platform != "linux", reason="requires GNU linker fault wrappers")


@pytest.fixture(scope="module")
def fault_core(tmp_path_factory):
    source = Path(__file__).parent / "host_ideas/store_faults.c"
    names = ("malloc", "calloc", "realloc", "fopen", "lstat", "opendir", "fread", "ferror", "fclose")
    lib = build_core(tmp_path_factory.mktemp("store-fault-core"), (source,),
                     tuple("-Wl,--wrap=" + name for name in names))
    lib.store_fault_io.argtypes = [C.c_uint, C.c_char_p]
    lib.store_fault_io_at.argtypes = [C.c_uint, C.c_char_p, C.c_uint]
    return lib


def snapshot(path):
    return {p.relative_to(path): hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None
            for p in path.rglob("*")}


@pytest.fixture(scope="module")
def generations(fault_core, tmp_path_factory):
    path = tmp_path_factory.mktemp("generations")
    assert save(fault_core, path, "1.0.0")[0] == 0
    assert save(fault_core, path, "2.0.0")[0] == 0
    return path


def test_allocation_failures_keep_new_generation(fault_core, generations, tmp_path):
    lib = fault_core
    reference = tmp_path / "reference"
    shutil.copytree(generations, reference)
    package = Package()
    lib.store_fault_allocation(0)
    assert lib.ideas_store_prepare(str(reference).encode(), b"042731", C.byref(package)) == 0
    total = lib.store_fault_allocations()
    lib.store_fault_reset()
    failures = []
    for nth in range(1, total + 1):
        root = tmp_path / str(nth)
        shutil.copytree(generations, root)
        before = snapshot(root)
        lib.store_fault_allocation(nth)
        result = lib.ideas_store_prepare(str(root).encode(), b"042731", C.byref(package))
        hits = lib.store_fault_hits()
        lib.store_fault_reset()
        if hits and (result == 0 or snapshot(root) != before):
            failures.append((nth, result))
        assert lib.ideas_store_prepare(str(root).encode(), b"042731", C.byref(package)) == 0
        if package.version != b"2.0.0": failures.append((nth, package.version.decode()))
    print("recovery allocation points", total, "unsafe failures", len(failures))
    assert total > 30 and not failures, failures[:20]


@pytest.mark.parametrize("kind,target", [
    (1, "/current/manifest.json"), (1, "/current/firmware/manifest.json"),
    (1, "/current/firmware/app.bin"), (2, "/current"),
    (3, "/current"), (4, "/current/manifest.json"), (4, "/current/firmware/app.bin"),
])
def test_io_failure_does_not_roll_back(fault_core, generations, tmp_path, kind, target):
    lib = fault_core
    shutil.copytree(generations, tmp_path, dirs_exist_ok=True)
    before = snapshot(tmp_path)
    package = Package()
    lib.store_fault_io(kind, target.encode())
    result = lib.ideas_store_prepare(str(tmp_path).encode(), b"042731", C.byref(package))
    hits = lib.store_fault_hits()
    lib.store_fault_reset()
    assert hits == 1
    assert result != 0 and snapshot(tmp_path) == before
    assert lib.ideas_store_prepare(str(tmp_path).encode(), b"042731", C.byref(package)) == 0
    assert package.version == b"2.0.0"


def test_stat_error_cannot_remove_current_journal(fault_core, generations, tmp_path):
    lib = fault_core
    shutil.copytree(generations, tmp_path, dirs_exist_ok=True)
    package = Package()
    assert lib.ideas_store_prepare(str(tmp_path).encode(), b"042731", C.byref(package)) == 0
    before = snapshot(tmp_path)
    lib.store_fault_io(2, b"/current")
    result = lib.ideas_store_recover(str(tmp_path).encode(), b"042731")
    lib.store_fault_reset()
    assert result != 0 and snapshot(tmp_path) == before


def test_delete_marker_read_error_cannot_restore_previous(fault_core, generations, tmp_path):
    lib = fault_core
    shutil.copytree(generations, tmp_path, dirs_exist_ok=True)
    base = tmp_path / "042731"
    (base / "current").rename(base / ".deleted")
    before = snapshot(tmp_path)
    package = Package()
    lib.store_fault_io(2, b"/.deleted")
    result = lib.ideas_store_prepare(str(tmp_path).encode(), b"042731", C.byref(package))
    lib.store_fault_reset()
    assert result != 0 and snapshot(tmp_path) == before
    assert lib.ideas_store_recover(str(tmp_path).encode(), b"042731") == 0
    assert not base.exists()


@pytest.mark.parametrize("failure", ["read", "allocation"])
def test_journal_error_cannot_change_application_identity(fault_core, generations, tmp_path, failure):
    lib = fault_core
    shutil.copytree(generations, tmp_path, dirs_exist_ok=True)
    package = Package()
    assert lib.ideas_store_prepare(str(tmp_path).encode(), b"042731", C.byref(package)) == 0
    before = snapshot(tmp_path)
    document, payloads = descriptor("3.0.0")
    document["application"]["id"] = "unrelated-application"
    result, package, raw = parse(lib, document, payloads=payloads)
    assert result == 0
    fetch, verify = callbacks(payloads)
    if failure == "read": lib.store_fault_io(1, b"/.roots")
    else: lib.store_fault_allocation(1)
    result = lib.ideas_store_download(str(tmp_path).encode(), raw, len(raw), C.byref(package),
                                     fetch, verify, None, None)
    hits = lib.store_fault_hits()
    lib.store_fault_reset()
    assert hits == 1 and result != 0 and snapshot(tmp_path) == before


def test_commit_probe_error_keeps_old_root_authorized(fault_core, generations, tmp_path):
    lib = fault_core
    shutil.copytree(generations, tmp_path, dirs_exist_ok=True)
    package = Package()
    assert lib.ideas_store_prepare(str(tmp_path).encode(), b"042731", C.byref(package)) == 0
    before = snapshot(tmp_path)
    document, payloads = descriptor("3.0.0")
    result, package, raw = parse(lib, document, payloads=payloads)
    assert result == 0
    fetch, verify = callbacks(payloads)
    # Preflight and metadata read are the first two probes; the third decides
    # which old root the commit journal must retain.
    lib.store_fault_io_at(5, b"/current", 3)
    result = lib.ideas_store_download(str(tmp_path).encode(), raw, len(raw), C.byref(package),
                                     fetch, verify, None, None)
    hits = lib.store_fault_hits()
    lib.store_fault_reset()
    assert hits == 1 and result != 0 and snapshot(tmp_path) == before
    assert lib.ideas_store_prepare(str(tmp_path).encode(), b"042731", C.byref(package)) == 0
    assert package.version == b"2.0.0"

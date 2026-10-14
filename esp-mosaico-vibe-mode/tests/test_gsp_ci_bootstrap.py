"""Exercise Windows release extraction on every CI host."""
import importlib.util
import io
from pathlib import Path
import zipfile

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "tools/ci/prepare_gsp.py"
SPEC = importlib.util.spec_from_file_location("prepare_gsp", SCRIPT)
bootstrap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bootstrap)


def archive_with(name, data):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr(name, data)
    return output.getvalue()


def test_windows_zip_copies_the_opened_binary_stream(tmp_path):
    payload = b"MZ\x00\xff\x10Windows release bytes"
    bootstrap.extract_verified_zip(archive_with("bin/gspc.exe", payload), "zip", tmp_path)
    assert (tmp_path / "bin/gspc.exe").read_bytes() == payload


def test_windows_zip_rejects_paths_outside_destination(tmp_path):
    with pytest.raises(ValueError, match="Unsafe GSP archive path"):
        bootstrap.extract_verified_zip(archive_with("../escaped.exe", b"payload"), "zip", tmp_path)
    assert not (tmp_path.parent / "escaped.exe").exists()

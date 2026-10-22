"""Keep the published Recovery release, image descriptor and manifest in sync."""
import hashlib
import json
from pathlib import Path


def test_prebuilt_recovery_release():
    project = Path(__file__).resolve().parents[1] / "firmware/vibe-mode"
    directory = project / "prebuilt/recovery"
    manifest = json.loads((directory / "manifest.json").read_text())
    assert manifest["schema_version"] == 3
    assert manifest["version"] == "0.2.1"
    assert manifest["target"] == "esp32s31"
    assert manifest["compatibility"]["recovery_abi"] == 2
    assert manifest["compatibility"]["layout_id"] == "mosaico-retained-test-2m-v2"
    assert manifest["layout"]["recovery_partition"]["subtype"] == "test"
    assert manifest["initial_boot"]["partition"] == "vibe_mode"
    assert manifest["initial_boot"]["expected_mode"] == "recovery"
    offsets = {"bootloader": 0x2000, "partition_table": 0x8000,
               "ota_data": 0x9000, "recovery": 0x20000}
    for name, offset in offsets.items():
        item = manifest["images"][name]
        data = (directory / item["file"]).read_bytes()
        assert int(item["offset"], 0) == offset
        assert len(data) == item["size"]
        assert hashlib.sha256(data).hexdigest() == item["sha256"]
    recovery = (directory / manifest["images"]["recovery"]["file"]).read_bytes()
    assert recovery[0] == 0xE9
    assert int.from_bytes(recovery[32:36], "little") == 0xABCD5432
    assert recovery[48:80].split(b"\0", 1)[0] == b"0.2.1"
    assert recovery[80:112].split(b"\0", 1)[0].decode() == manifest["project"]
    assert recovery[213] == 1  # app descriptor's QIO Flash mode
    assert len(recovery) <= 0x1C0000
    assert (directory / "bootloader.bin").stat().st_size <= 0x6000
    ota_data = (directory / "ota_data_initial.bin").read_bytes()
    assert ota_data == b"\xff" * 32 + b"MOSAICO-BOOT-02!" + b"\xff" * (0x2000 - 48)
    # A release update must not silently alter the retained Recovery layout.
    assert manifest["images"]["partition_table"]["sha256"] == (
        "ff7ac35a4f2e51424150a6ba56172174299baa737230a09792dd46e8f5d0bb99"
    )


def test_old_reviewed_bundle_is_not_accepted_by_02_tools(tmp_path):
    import sys

    import pytest

    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root.parent / "mosaico-tools/tools"))
    from mosaico_cli.errors import EnvironmentError
    from mosaico_cli.recovery import load_bundle
    (tmp_path / "manifest.json").write_text(json.dumps({"schema_version": 2}))
    with pytest.raises(EnvironmentError, match="schema is obsolete"):
        load_bundle(tmp_path, "esp32s31")


def test_reviewed_bundle_is_accepted_by_02_tools():
    import sys

    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root.parent / "mosaico-tools/tools"))
    from mosaico_cli.recovery import load_bundle

    manifest = load_bundle(root / "firmware/vibe-mode/prebuilt/recovery", "esp32s31")
    assert manifest["version"] == "0.2.1"

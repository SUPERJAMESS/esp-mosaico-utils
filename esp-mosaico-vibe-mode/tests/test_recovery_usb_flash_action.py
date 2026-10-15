"""The product's bundle target must retain Iris's native USB identity handoff."""
import importlib.util
import sys
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace

import pytest


@pytest.mark.parametrize("identity_matches", [True, False])
@pytest.mark.skipif(sys.platform == "win32", reason="POSIX host-worker descriptor handoff")
def test_recovery_target_uses_verified_usb_handoff(monkeypatch, identity_matches):
    project = Path(__file__).resolve().parents[1] / "firmware/vibe-mode"
    spec = importlib.util.spec_from_file_location("vibe_idf_ext", project / "idf_ext.py")
    extension = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extension)
    import iris_usb_flash as usb

    events = []
    port = SimpleNamespace(device="console")
    monkeypatch.setattr(usb, "usb_config", lambda _: {
        "ESP_IRIS_USB_VID": 0x303A, "ESP_IRIS_USB_PID": 0x4002})
    monkeypatch.setattr(usb, "select_port", lambda *args: port)
    monkeypatch.setattr(usb, "claim_device", lambda _: ExitStack())
    monkeypatch.setattr(usb, "wait_for_port", lambda *args, **kwargs: port)

    def prepare(*args):
        events.append("verify-rom")
        if not identity_matches:
            raise usb.UsbFlashError("ROM identity mismatch")
        return SimpleNamespace(device="rom"), "30eda0f40c10"

    monkeypatch.setattr(usb, "prepare_rom", prepare)
    monkeypatch.setitem(sys.modules, "idf_py_actions.errors", SimpleNamespace(FatalError=RuntimeError))
    monkeypatch.setitem(sys.modules, "idf_py_actions.tools", SimpleNamespace(
        ensure_build_directory=lambda *args: None,
        run_target=lambda target, args: events.append(target)))
    native = {"callback": lambda action, ctx, args, **kwargs:
              events.append((action, args.port, kwargs)), "options": [{"names": ["--all"]}],
              "dependencies": ["all"]}
    base = {"actions": {"flash": native}}
    result = extension.action_extensions(base, str(project))
    assert result["version"]
    registered = result["actions"]["mosaico-recover-flash"]
    assert registered["options"] == native["options"]
    assert registered["dependencies"] == native["dependencies"]
    args = SimpleNamespace(build_dir="build", port="console")
    ctx = SimpleNamespace(info_name="idf.py")
    if identity_matches:
        registered["callback"]("mosaico-recover-flash", ctx, args, flash_all=True)
        assert events == ["all", "verify-rom", ("mosaico-recover-flash", "rom", {"flash_all": True})]
    else:
        with pytest.raises(RuntimeError, match="ROM identity mismatch"):
            registered["callback"]("mosaico-recover-flash", ctx, args)
        assert events == ["all", "verify-rom"]

"""Native flash/monitor must follow one device and preserve IDF's write target."""
from __future__ import annotations

import ast
import json
import sys
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace

import iris_usb_flash as integration
import pytest
from iris_usb_flash import ports as usb

VID, PID = 0x303A, 0x4002
MAC = "30eda0f4518e"


def port(device="/dev/ttyACM0", *, role="console", mac=MAC, location="1-2:1.0", rom=False):
    return usb.Port(usb.canonical_port(device), VID, 0x0020 if rom else PID, mac, location,
                    "" if rom else "ESP-Iris 0.2 " + role)


@pytest.fixture
def world(monkeypatch):
    available = [port(), port("/dev/ttyACM1", role="data", location="1-2:1.2")]
    monkeypatch.setattr(usb, "ports", lambda: available)
    return available


def test_select_console_and_refuse_ambiguous_or_data(world):
    assert usb.select_port(None, VID, PID) == world[0]
    with pytest.raises(usb.UsbFlashError, match="data interface"):
        usb.select_port("/dev/ttyACM1", VID, PID)
    world.append(port("/dev/ttyACM2", mac="30eda0f40c10", location="1-7:1.0"))
    with pytest.raises(usb.UsbFlashError, match="Select the board"):
        usb.select_port(None, VID, PID)
    assert usb.select_port("/dev/ttyACM2", VID, PID) == world[2]
    assert usb.select_port("/dev/ttyUSB0", VID, PID) is None


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX serial symlink")
def test_stable_alias_selects_console(world, tmp_path):
    alias = tmp_path / "board"
    alias.symlink_to(world[0].device)
    assert usb.select_port(str(alias), VID, PID) == world[0]


def test_wait_requires_topology_role_and_mac(world):
    origin = world[0]
    world[:] = [port("/dev/ttyACM9", mac="30eda0f40c10", location="1-7:1.0"),
                port("/dev/ttyACM8", role="data", location="1-2:1.2"),
                port("/dev/ttyACM7")]
    assert usb.wait_for_port(origin, VID, PID, rom=False, expected_mac=MAC) == world[2]
    world.append(port("/dev/ttyACM10"))
    with pytest.raises(usb.UsbFlashError, match="refusing to guess"):
        usb.wait_for_port(origin, VID, PID, rom=False, expected_mac=MAC)


def test_rom_mac_mismatch_prevents_handoff(monkeypatch):
    origin, rom = port(), port("/dev/ttyACM4", rom=True, mac="30:ed:a0:f4:0c:10")
    calls = []
    monkeypatch.setattr(usb, "enter_download", lambda p: calls.append(p))
    monkeypatch.setattr(usb, "wait_for_port", lambda *a, **kw: rom)
    with pytest.raises(usb.UsbFlashError, match="no flash performed"):
        usb.prepare_rom(origin, VID, PID)
    assert calls == [origin]


def test_rom_start_uses_hardware_identity_without_reset(monkeypatch):
    rom = port(rom=True, mac="30:ed:a0:f4:51:8e")
    monkeypatch.setattr(usb, "enter_download", lambda p: pytest.fail("already in ROM"))
    monkeypatch.setattr(usb.serial, "Serial", lambda *a, **kw: pytest.fail("must not open ROM before flash"))
    assert usb.prepare_rom(rom, VID, PID) == (rom, MAC)


def test_missing_topology_does_not_reset(monkeypatch):
    monkeypatch.setattr(usb, "enter_download", lambda p: pytest.fail("must not reset"))
    with pytest.raises(usb.UsbFlashError, match="physical location"):
        usb.prepare_rom(port(location=""), VID, PID)


def test_reenumeration_timeout_is_bounded(world, monkeypatch):
    monkeypatch.setattr(usb.time, "monotonic", iter([0, 0, 16]).__next__)
    monkeypatch.setattr(usb.time, "sleep", lambda _: None)
    with pytest.raises(usb.UsbFlashError, match="manual Boot/ROM"):
        usb.wait_for_port(world[0], VID, PID, rom=True)


def test_prepare_rom_uses_descriptor_without_opening_rom(monkeypatch):
    origin, rom = port(), port("/dev/ttyACM4", rom=True, mac="30:ED:A0:F4:51:8E")
    requests = []
    monkeypatch.setattr(usb, "enter_download", lambda p: requests.append(p))
    monkeypatch.setattr(usb, "wait_for_port", lambda *a, **kw: rom)
    monkeypatch.setattr(usb.serial, "Serial", lambda *a, **kw: pytest.fail("extra ROM session would reset it"))
    assert usb.prepare_rom(origin, VID, PID) == (rom, MAC)
    assert requests == [origin]


@pytest.mark.parametrize("value", ["", "not-a-mac", "30:ed:a0:f4:51:8e:00:00", "junk30eda0f4518e"])
def test_missing_or_invalid_rom_identity_prevents_flash(value):
    with pytest.raises(usb.UsbFlashError, match="cannot verify"):
        usb.prepare_rom(port(rom=True, mac=value), VID, PID)


def test_handshake_initializes_lines_before_open_and_closes(monkeypatch):
    events = []

    class Serial:
        def __init__(self, **kwargs):
            assert kwargs["port"] is None

        def __setattr__(self, name, value):
            events.append((name, value))
            object.__setattr__(self, name, value)

        def open(self):
            events.append(("open", self.dtr, self.rts))

        def close(self):
            events.append(("close",))

    monkeypatch.setattr(usb.serial, "Serial", Serial)
    monkeypatch.setattr(usb, "execute_reset", lambda device, steps: events.append(("sequence", steps)))
    usb.enter_download(port())
    assert events[3] == ("open", True, True)
    assert events[-1] == ("close",)
    assert events[4][1] == usb.reset_steps("rom", "usb_serial_jtag")


@pytest.fixture
def actions(tmp_path, monkeypatch):
    events = []
    cfg = {"IDF_TARGET": "esp32s31", "ESP_IRIS_ENABLE": True,
           "ESP_IRIS_TRANSPORT_USB": True, "ESP_IRIS_USB_AUTO_DOWNLOAD": True,
           "ESP_IRIS_USB_VID": VID, "ESP_IRIS_USB_PID": PID}
    config_file = tmp_path / "config/sdkconfig.json"
    config_file.parent.mkdir()
    config_file.write_text(json.dumps(cfg))

    class FatalError(RuntimeError):
        pass

    monkeypatch.setitem(sys.modules, "idf_py_actions.errors", SimpleNamespace(FatalError=FatalError))
    monkeypatch.setitem(sys.modules, "idf_py_actions.tools", SimpleNamespace(
        ensure_build_directory=lambda *a: events.append("configure"),
        run_target=lambda name, args: events.append("build")))
    monkeypatch.setattr(integration, "select_port", lambda *a: port())
    monkeypatch.setattr(integration, "claim_device", lambda p: ExitStack())

    def prepare(*args):
        events.append("rom")
        return port("/dev/ttyACM6", rom=True), MAC

    monkeypatch.setattr(integration, "prepare_rom", prepare)
    monkeypatch.setattr(integration, "wait_for_port", lambda *a, **kw: port("/dev/ttyACM8"))

    def original(action, ctx, args, **kwargs):
        events.append((action, args.port, kwargs))

    base = {"actions": {name: {"callback": original, "options": ["untouched"]}
                        for name in ["flash", "app-flash", "encrypted-flash", "encrypted-app-flash", "monitor"]}}
    integration.action_extensions(base, str(tmp_path))
    args = SimpleNamespace(build_dir=str(tmp_path), port="/dev/ttyACM0")
    ctx = SimpleNamespace(info_name="idf.py")
    return base, args, ctx, events, config_file


@pytest.mark.parametrize("target", ["flash", "app-flash", "encrypted-flash", "encrypted-app-flash"])
def test_flash_builds_before_reset_and_preserves_target_options(actions, target):
    base, args, ctx, events, _ = actions
    flags = {"flash_all": True, "trace": True, "extra_args": "--verify"}
    base["actions"][target]["callback"](target, ctx, args, **flags)
    base["actions"]["monitor"]["callback"]("monitor", ctx, args, no_reset=False, timestamps=True)
    assert events == ["configure", "build", "rom", (target, port("/dev/ttyACM6").device, flags),
                      ("monitor", port("/dev/ttyACM8").device, {"no_reset": True, "timestamps": True})]
    assert base["actions"][target]["options"] == ["untouched"]


def test_extension_loads_once_and_standalone_monitor_is_unchanged(actions):
    base, args, ctx, events, _ = actions
    original = base["actions"]["flash"]["callback"]
    integration.action_extensions(base, "unused")
    assert base["actions"]["flash"]["callback"] is original
    base["actions"]["monitor"]["callback"]("monitor", ctx, args, no_reset=False)
    assert events == [("monitor", "/dev/ttyACM0", {"no_reset": False})]


def test_disabled_profile_keeps_native_behavior(actions):
    base, args, ctx, events, config_file = actions
    config_file.write_text("{}")
    base["actions"]["flash"]["callback"]("flash", ctx, args)
    assert events == ["configure", ("flash", "/dev/ttyACM0", {})]


def test_failed_build_never_resets_device(actions, monkeypatch):
    base, args, ctx, events, _ = actions

    def failed(*a):
        raise RuntimeError("build failed")

    monkeypatch.setattr(sys.modules["idf_py_actions.tools"], "run_target", failed)
    with pytest.raises(RuntimeError, match="build failed"):
        base["actions"]["flash"]["callback"]("flash", ctx, args)
    assert events == ["configure"]


def test_failed_identity_never_calls_native_flash(actions, monkeypatch):
    base, args, ctx, events, _ = actions

    def failed(*a):
        raise usb.UsbFlashError("wrong hardware")

    monkeypatch.setattr(integration, "prepare_rom", failed)
    with pytest.raises(RuntimeError, match="wrong hardware"):
        base["actions"]["flash"]["callback"]("flash", ctx, args)
    assert events == ["configure", "build"]


def test_python38_syntax():
    for source in Path(integration.__file__).parent.glob("*.py"):
        ast.parse(source.read_text(), feature_version=(3, 8))


def test_busy_data_interface_prevents_claim_and_releases_console(world, monkeypatch, tmp_path):
    monkeypatch.setattr("iris_gateway.link.tempfile.gettempdir", lambda: str(tmp_path))
    busy = usb.EndpointLock("usb:location=1-2:1.2")
    busy.acquire()
    try:
        with pytest.raises(usb.UsbFlashError, match="board is in use"):
            usb.claim_device(world[0])
        console = usb.EndpointLock("usb:location=1-2:1.0")
        try:
            console.acquire()
        finally:
            console.close()
    finally:
        busy.close()
    with usb.claim_device(world[0]):
        root = tmp_path / "esp-iris-locks"
        assert usb.EndpointLock.held("usb:location=1-2:1.0", root)
        assert usb.EndpointLock.held("usb:location=1-2:1.2", root)


def test_conflicting_flags_fail_before_build_or_reset(actions):
    base, args, ctx, events, _ = actions
    with pytest.raises(RuntimeError, match="cannot be combined"):
        base["actions"]["flash"]["callback"]("flash", ctx, args,
                                               flash_all=True, trust_flash_content=True)
    assert events == ["configure"]


def test_replaced_board_during_build_is_not_reset(actions, monkeypatch):
    base, args, ctx, events, _ = actions
    selected = iter([port(), port(mac="30eda0f40c10")])
    monkeypatch.setattr(integration, "select_port", lambda *a: next(selected))
    with pytest.raises(RuntimeError, match="device changed during the build"):
        base["actions"]["flash"]["callback"]("flash", ctx, args)
    assert events == ["configure", "build"]


@pytest.mark.parametrize("failure", ["prepare", "flash", "wait", "monitor"])
def test_failures_release_device_claim(actions, monkeypatch, failure):
    base, args, ctx, _events, _ = actions
    released = []
    claim = ExitStack()
    claim.callback(lambda: released.append(True))
    monkeypatch.setattr(integration, "claim_device", lambda p: claim)

    def fail(*a, **kw):
        raise usb.UsbFlashError("injected failure")

    if failure == "prepare":
        monkeypatch.setattr(integration, "prepare_rom", fail)
    elif failure == "wait":
        monkeypatch.setattr(integration, "wait_for_port", fail)
    elif failure in ("flash", "monitor"):
        # Install failing native actions before wrapping them.
        original = base["actions"][failure]["callback"]
        base["actions"][failure]["callback"] = fail
        # Fresh wrappers must share their own flash-to-monitor state.
        for name in base["actions"]:
            if name != failure:
                base["actions"][name]["callback"] = lambda *a, **kw: None
        integration.action_extensions(base, "unused")
        assert base["actions"][failure]["callback"] is not original
    with pytest.raises(RuntimeError, match="injected failure"):
        base["actions"]["flash"]["callback"]("flash", ctx, args)
        assert failure == "monitor" and not released
        base["actions"]["monitor"]["callback"]("monitor", ctx, args)
    assert released == [True]

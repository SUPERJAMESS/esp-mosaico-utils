"""Replay IORegistry trees without opening USB or requiring a macOS runner."""
import plistlib
from types import SimpleNamespace

import pytest

from iris_gateway import discovery, usb_descriptors
from iris_usb_flash import ports as flash


def board(name, numbers=(0, 2), pid=0x4002):
    return {"idVendor": 0x303A, "idProduct": pid, "IORegistryEntryChildren": [
        {"bInterfaceNumber": number, "IORegistryEntryChildren": [
            {"IOCalloutDevice": f"/dev/cu.{name}{number}"}]} for number in numbers]}


@pytest.mark.parametrize("reverse", [False, True])
def test_composite_interfaces_share_descriptors_across_discovery_and_flash(monkeypatch, reverse):
    ports = [SimpleNamespace(device=f"/dev/cu.{name}{number}", vid=0x303A, pid=0x4002,
                             serial_number=mac, location=location, interface=None, product="ESP-Iris")
             for name, mac, location in (("a", "30eda0f4518e", "1-2"), ("b", "30eda0f40c10", "1-3"))
             for number in (0, 2)]
    monkeypatch.setattr(usb_descriptors.sys, "platform", "darwin")
    monkeypatch.setattr(usb_descriptors.list_ports, "comports", lambda: ports[::-1] if reverse else ports)
    monkeypatch.setattr(usb_descriptors.subprocess, "run", lambda *a, **kw:
                        SimpleNamespace(stdout=plistlib.dumps([board("b"), board("a")])))
    devices = discovery.discover_iris_usb_devices()
    assert [d.link_role for d in devices] == ["control", "data", "control", "data"]
    endpoints = [discovery.usb_endpoint(discovery.resolve_usb_port(d.path)) for d in devices]
    assert len(set(endpoints)) == 4
    for device, endpoint in zip(devices, endpoints):
        assert discovery.resolve_usb_port(endpoint)["path"] == device.path
        assert discovery.resolve_usb_port(device.path.replace("/dev/cu.", "/dev/tty."))["path"] == device.path
    assert flash.select_port("/dev/cu.a0", 0x303A, 0x4002).serial == "30eda0f4518e"
    with pytest.raises(flash.UsbFlashError, match="data interface"):
        flash.select_port("/dev/cu.a2", 0x303A, 0x4002)


def test_missing_registry_never_collapses_or_opens_unknown_cdc(monkeypatch):
    ports = [SimpleNamespace(device=f"/dev/cu.a{number}", vid=0x303A, pid=0x4002,
                             serial_number="same", location="1-2", interface=None, product="ESP-Iris")
             for number in (0, 2)]
    monkeypatch.setattr(usb_descriptors.sys, "platform", "darwin")
    monkeypatch.setattr(usb_descriptors.list_ports, "comports", lambda: ports)
    monkeypatch.setattr(usb_descriptors, "_mac_interfaces", dict)
    metadata = [discovery.resolve_usb_port(port.device) for port in ports]
    assert len({discovery.usb_endpoint(item) for item in metadata}) == 2
    assert all(item["link_role"] == "unknown" for item in metadata)
    assert not any(discovery.iris_usb_allowed(item, explicit=True) for item in metadata)


def test_registry_interfaces_do_not_leak_between_nested_devices():
    tree = board("a")
    tree["IORegistryEntryChildren"].append(board("rom", numbers=(0,), pid=0x0020))
    result = usb_descriptors.registry_interfaces([tree])
    assert result["/dev/cu.a2"]["interface"] == "ESP-Iris 0.2 data"
    assert result["/dev/cu.rom0"]["interface"] == ""


def test_conflicting_registry_role_is_unknown():
    tree = board("a")
    tree["IORegistryEntryChildren"][0]["USB Interface Name"] = "ESP-Iris 0.2 data"
    assert usb_descriptors.registry_interfaces([tree])["/dev/cu.a0"]["interface"] == ""


@pytest.mark.parametrize("payload", [b"not a plist", b'<?xml version="1.0"?><plist><broken>'])
def test_invalid_registry_snapshot_degrades_to_unknown(monkeypatch, payload):
    monkeypatch.setattr(usb_descriptors.subprocess, "run", lambda *a, **kw: SimpleNamespace(stdout=payload))
    assert usb_descriptors._mac_interfaces() == {}


def test_callout_and_tty_aliases_are_enumerated_once(monkeypatch):
    monkeypatch.setattr(usb_descriptors.sys, "platform", "darwin")
    monkeypatch.setattr(usb_descriptors, "_mac_interfaces", dict)
    monkeypatch.setattr(usb_descriptors.list_ports, "comports", lambda: [
        SimpleNamespace(device=path, vid=0x303A, pid=0x0020, location="1-2", interface=None)
        for path in ("/dev/tty.usbmodem1", "/dev/cu.usbmodem1")])
    assert [p.device for p in usb_descriptors.serial_ports()] == ["/dev/cu.usbmodem1"]

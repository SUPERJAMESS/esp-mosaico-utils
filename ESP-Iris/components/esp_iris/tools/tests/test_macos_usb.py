"""Replay IORegistry trees without opening USB or requiring a macOS runner."""
import copy
import json
import plistlib
from pathlib import Path
from types import SimpleNamespace

import pytest

from iris_gateway import discovery, usb_descriptors
from iris_usb_flash import ports as flash


def board(name, numbers=(0, 2), pid=0x4002):
    device = {"IOObjectClass": "IOUSBHostDevice", "idVendor": 0x303A,
              "idProduct": pid, "IORegistryEntryChildren": []}
    for number in numbers:
        if pid != 0x4002:
            device["IORegistryEntryChildren"].append({
                "IOObjectClass": "IOUSBHostInterface", "bInterfaceNumber": number,
                "IORegistryEntryChildren": [{"IOCalloutDevice": f"/dev/cu.{name}{number}"}]})
            continue
        device["IORegistryEntryChildren"].extend([
            {"IOObjectClass": "IOUSBHostInterface", "idVendor": 0x303A, "idProduct": pid,
             "bInterfaceNumber": number, "bInterfaceClass": 2,
             "kUSBString": {0: "ESP-Iris 0.2 console", 2: "ESP-Iris 0.2 data"}[number]},
            {"IOObjectClass": "IOUSBHostInterface", "idVendor": 0x303A, "idProduct": pid,
             "bInterfaceNumber": number + 1, "bInterfaceClass": 10,
             "IORegistryEntryChildren": [
                 {"IOObjectClass": "AppleUSBACMData", "idVendor": 0x303A, "idProduct": pid,
                  "IORegistryEntryChildren": [{"IOObjectClass": "IOSerialBSDClient",
                                              "IOCalloutDevice": f"/dev/cu.{name}{number + 1}"}]}]},
        ])
    return device


@pytest.mark.parametrize("reverse", [False, True])
def test_composite_interfaces_share_descriptors_across_discovery_and_flash(monkeypatch, reverse):
    ports = [SimpleNamespace(device=f"/dev/cu.{name}{number}", vid=0x303A, pid=0x4002,
                             serial_number=mac, location=location, interface=None, product="ESP-Iris")
             for name, mac, location in (("a", "30eda0f4518e", "1-2"), ("b", "30eda0f40c10", "1-3"))
             for number in (1, 3)]
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
    assert flash.select_port("/dev/cu.a1", 0x303A, 0x4002).serial == "30eda0f4518e"
    with pytest.raises(flash.UsbFlashError, match="data interface"):
        flash.select_port("/dev/cu.a3", 0x303A, 0x4002)


def test_missing_registry_never_collapses_or_opens_unknown_cdc(monkeypatch):
    ports = [SimpleNamespace(device=f"/dev/cu.a{number}", vid=0x303A, pid=0x4002,
                             serial_number="same", location="1-2", interface=None, product="ESP-Iris")
             for number in (1, 3)]
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
    assert result["/dev/cu.a3"]["interface"] == "ESP-Iris 0.2 data"
    assert result["/dev/cu.rom0"]["interface"] == ""


def test_conflicting_registry_role_is_unknown():
    tree = board("a")
    tree["IORegistryEntryChildren"][0]["USB Interface Name"] = "ESP-Iris 0.2 data"
    assert usb_descriptors.registry_interfaces([tree])["/dev/cu.a1"]["interface"] == ""


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


@pytest.mark.parametrize("legacy", [False, True])
def test_actual_macos_driver_tree_preserves_named_cdc_functions(monkeypatch, legacy):
    capture = Path(__file__).parent / "fixtures/macos-iris-usb.json"
    tree = json.loads(capture.read_text())
    if legacy:
        def convert(node):
            if node.get("IOObjectClass") == "IOUSBHostDevice":
                node["IOObjectClass"] = "IOUSBDevice"
            if node.get("IOObjectClass") == "IOUSBHostInterface":
                node["IOObjectClass"] = "IOUSBInterface"
            for child in node.get("IORegistryEntryChildren", []):
                convert(child)
        convert(tree[0])
    descriptors = usb_descriptors.registry_interfaces(tree)
    console = "/dev/cu.usbmodemfixture-a1"
    data = "/dev/cu.usbmodemfixture-a3"
    assert descriptors[console] == {"interface": "ESP-Iris 0.2 console", "number": 0}
    assert descriptors[data] == {"interface": "ESP-Iris 0.2 data", "number": 2}
    ports = [SimpleNamespace(device=path, vid=0x303A, pid=0x4002, serial_number="001122334455",
                             location="1-2", interface=None, product="ESP-Iris")
             for path in (console, data)]
    monkeypatch.setattr(usb_descriptors.sys, "platform", "darwin")
    monkeypatch.setattr(usb_descriptors.list_ports, "comports", lambda: ports)
    monkeypatch.setattr(usb_descriptors, "_mac_interfaces", lambda: descriptors)
    devices = discovery.discover_iris_usb_devices()
    assert [device.link_role for device in devices] == ["control", "data"]
    metadata = discovery.resolve_usb_port(console)
    assert discovery.iris_usb_allowed(metadata, explicit=True)
    assert discovery.usb_endpoint(metadata) == "usb:location=1-2:1.0"
    assert discovery.usb_endpoint(discovery.resolve_usb_port(data)) == "usb:location=1-2:1.2"
    assert flash.select_port(console, 0x303A, 0x4002).interface == "ESP-Iris 0.2 console"
    with pytest.raises(flash.UsbFlashError, match="data interface"):
        flash.select_port(data, 0x303A, 0x4002)
    assert all(port.interface is None and port.location == "1-2" for port in ports)


@pytest.mark.parametrize("damage", ["missing_control", "missing_data", "unnamed_control",
                                  "wrong_control_class", "wrong_data_class", "duplicate_data"])
def test_incomplete_or_ambiguous_cdc_pair_stays_unknown(damage):
    tree = board("a", numbers=(0,))
    interfaces = tree["IORegistryEntryChildren"]
    if damage == "missing_control":
        interfaces.pop(0)
    elif damage == "missing_data":
        interfaces.pop(1)
    elif damage == "unnamed_control":
        interfaces[0].pop("kUSBString")
    elif damage == "wrong_control_class":
        interfaces[0]["bInterfaceClass"] = 10
    elif damage == "wrong_data_class":
        interfaces[1]["bInterfaceClass"] = 2
    else:
        interfaces.append(copy.deepcopy(interfaces[1]))
    descriptors = usb_descriptors.registry_interfaces([tree])
    assert not any(item["interface"] for item in descriptors.values())


def test_named_control_cannot_borrow_data_interface_from_nested_device():
    parent = board("parent", numbers=(0,))
    child = board("child", numbers=(0,))
    parent["IORegistryEntryChildren"].pop(1)
    child["IORegistryEntryChildren"].pop(0)
    parent["IORegistryEntryChildren"].append(child)
    descriptors = usb_descriptors.registry_interfaces([parent])
    assert descriptors["/dev/cu.child1"]["interface"] == ""

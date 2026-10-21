"""Windows CDC roles come from native interface devnodes, not COM ordering."""
from types import SimpleNamespace

import pytest

from iris_gateway import discovery, usb_descriptors


@pytest.mark.parametrize("reverse", [False, True])
def test_windows_console_and_data_share_physical_topology(monkeypatch, reverse):
    # Actual Windows shape: interface zero has neither interface nor location;
    # interface two has a location but no interface name. COM numbers interleave.
    ports = [SimpleNamespace(device=name, vid=0x303A, pid=0x4002,
                             serial_number="duplicate", location=location,
                             interface=None, product=None)
             for name, location in (("COM43", None), ("COM54", "1-8.2.1:x.2"),
                                    ("COM61", None), ("COM60", "1-8.3.1:x.2"))]
    descriptors = {
        name: {"number": number, "interface": usb_descriptors._IRIS_INTERFACES[number],
               "location": physical}
        for name, number, physical in (("COM43", 0, "pci.8.2.1"), ("COM54", 2, "pci.8.2.1"),
                                       ("COM61", 0, "pci.8.3.1"), ("COM60", 2, "pci.8.3.1"))
    }
    monkeypatch.setattr(usb_descriptors.sys, "platform", "win32")
    monkeypatch.setattr(usb_descriptors, "_windows_interfaces", lambda: descriptors)
    monkeypatch.setattr(usb_descriptors.list_ports, "comports", lambda: ports[::-1] if reverse else ports)
    devices = discovery.discover_iris_usb_devices()
    assert [(d.path, d.link_role) for d in devices] == [
        ("COM43", "control"), ("COM54", "data"), ("COM60", "data"), ("COM61", "control")]
    metadata = [discovery.resolve_usb_port(d.path) for d in devices]
    assert len({discovery.usb_endpoint(item) for item in metadata}) == 4
    assert metadata[0]["location"].split(":")[0] == metadata[1]["location"].split(":")[0]
    assert metadata[2]["location"].split(":")[0] == metadata[3]["location"].split(":")[0]
    assert metadata[0]["location"].split(":")[0] != metadata[2]["location"].split(":")[0]
    assert all(port.interface is None for port in ports)
    complete = {p.device: p.location for p in usb_descriptors._windows_ports(ports)}
    control_only = usb_descriptors._windows_ports([ports[0]])
    assert control_only[0].location == complete[ports[0].device]


def test_windows_fallback_topology_and_unrelated_ports(monkeypatch):
    ports = [SimpleNamespace(device="COM10", vid=0x303A, pid=0x4002, location=None),
             SimpleNamespace(device="COM11", vid=0x303A, pid=0x1001, location=None)]
    monkeypatch.setattr(usb_descriptors, "_windows_interfaces", lambda: {
        "COM10": {"number": 2, "interface": "ESP-Iris 0.2 data", "location": "pci.8.1"}})
    result = usb_descriptors._windows_ports(ports)
    assert result[0].location == "win-pci.8.1:1.2"
    assert usb_descriptors.link_role(result[0]) == "data"
    assert result[1] is ports[1]


def test_windows_registry_reads_function_devnodes_and_rejects_reused_com(monkeypatch):
    import sys

    base = r"SYSTEM\CurrentControlSet\Enum\USB\VID_303A&PID_4002&MI_"
    tree = {
        base + "00": {"a": {"LocationInformation": "pci.8.1", "Device Parameters": {"PortName": "COM43"}},
                       "stale": {"Device Parameters": {"PortName": "COM99"}}},
        base + "02": {"a": {"LocationInformation": "pci.8.1", "Device Parameters": {"PortName": "COM54"}},
                       "stale": {"Device Parameters": {"PortName": "COM99"}}},
    }

    class Key:
        def __init__(self, value): self.value = value
        def __enter__(self): return self
        def __exit__(self, *args): pass

    def open_key(parent, path):
        try:
            return Key((tree if parent is None else parent.value)[path])
        except KeyError:
            raise OSError("missing") from None

    def enum_key(key, index):
        try:
            return list(key.value)[index]
        except IndexError:
            raise OSError("done") from None

    def query(key, name):
        try:
            return key.value[name], 1
        except KeyError:
            raise OSError("missing") from None

    monkeypatch.setitem(sys.modules, "winreg", SimpleNamespace(
        HKEY_LOCAL_MACHINE=None, OpenKey=open_key, EnumKey=enum_key, QueryValueEx=query))
    descriptors = usb_descriptors._windows_interfaces()
    assert descriptors["COM43"]["number"] == 0
    assert descriptors["COM54"]["number"] == 2
    assert descriptors["COM43"]["location"] == descriptors["COM54"]["location"]
    assert "COM99" not in descriptors

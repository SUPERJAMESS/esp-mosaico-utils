"""Passive serial enumeration with interface-scoped macOS USB descriptors."""
from __future__ import annotations

import copy
import plistlib
import subprocess
import sys
from typing import Any
from xml.parsers.expat import ExpatError

from serial.tools import list_ports

_IRIS_INTERFACES = {0: "ESP-Iris 0.2 console", 2: "ESP-Iris 0.2 data"}


def registry_interfaces(tree: list[dict]) -> dict[str, dict[str, Any]]:
    """Associate each BSD node with its own USB interface, never a sibling."""
    found: dict[str, dict[str, Any]] = {}

    def visit(node: dict, device: dict, interface: dict) -> None:
        if "idVendor" in node and "idProduct" in node:
            device, interface = node, {}
        if "bInterfaceNumber" in node:
            interface = node
        path = node.get("IOCalloutDevice")
        if isinstance(path, str) and interface:
            number = interface.get("bInterfaceNumber")
            name = interface.get("USB Interface Name", "")
            if isinstance(number, int) and (device.get("idVendor"), device.get("idProduct")) == (0x303A, 0x4002):
                # These numbers and names are the owning firmware's descriptors.
                expected = _IRIS_INTERFACES.get(number, "")
                name = expected if not name or name == expected else ""
            found[path] = {"interface": name, "number": number}
        for child in node.get("IORegistryEntryChildren", []):
            if isinstance(child, dict):
                visit(child, device, interface)

    for root in tree:
        if isinstance(root, dict):
            visit(root, {}, {})
    return found


def _mac_interfaces() -> dict[str, dict[str, Any]]:
    # Read one snapshot per enumeration. No port is opened and no DTR/RTS changes.
    for device_class in ("IOUSBHostDevice", "IOUSBDevice"):
        try:
            result = subprocess.run(
                ["/usr/sbin/ioreg", "-a", "-l", "-r", "-c", device_class],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True, timeout=2,
            )
            tree = plistlib.loads(result.stdout)
            interfaces = registry_interfaces(tree) if isinstance(tree, list) else {}
            if interfaces:
                return interfaces
        except (OSError, subprocess.SubprocessError, ValueError, plistlib.InvalidFileException, ExpatError):
            continue
    return {}


def serial_ports() -> list[Any]:
    ports = list(list_ports.comports())
    if sys.platform != "darwin":
        return ports
    descriptors = _mac_interfaces() if ports else {}
    result = {}
    for original in ports:
        port = copy.copy(original)
        port.device = port.device.replace("/dev/tty.", "/dev/cu.")
        descriptor = descriptors.get(port.device, {})
        port.interface = descriptor.get("interface") or getattr(port, "interface", None) or ""
        number = descriptor.get("number")
        # pySerial's location identifies the USB device on macOS, not its CDC.
        if isinstance(number, int) and getattr(port, "location", None):
            port.location = str(port.location).split(":", 1)[0] + f":1.{number}"
        elif port.vid == 0x303A and port.pid == 0x4002:
            port.location = ""  # A BSD path is unambiguous; a shared location is not.
        if port.vid == 0x303A and port.pid == 0x4002:
            port.interface = descriptor.get("interface") or ""
            port.iris_link_role = {"ESP-Iris 0.2 console": "control", "ESP-Iris 0.2 data": "data"}.get(
                port.interface, "unknown")
        result[port.device] = port
    return list(result.values())


def link_role(port: Any) -> str:
    return getattr(port, "iris_link_role", None) or (
        "data" if getattr(port, "interface", None) == "ESP-Iris 0.2 data" else "control")

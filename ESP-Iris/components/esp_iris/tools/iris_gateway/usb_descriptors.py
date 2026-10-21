"""Passive serial enumeration with interface-scoped native USB metadata."""
from __future__ import annotations

import copy
import plistlib
import subprocess
import sys
from typing import Any
from xml.parsers.expat import ExpatError

from serial.tools import list_ports

_IRIS_INTERFACES = {0: "ESP-Iris 0.2 console", 2: "ESP-Iris 0.2 data"}
_USB_DEVICE_CLASSES = {"IOUSBHostDevice", "IOUSBDevice"}
_USB_INTERFACE_CLASSES = {"IOUSBHostInterface", "IOUSBInterface"}


def _interface_name(interface: dict) -> str:
    names = {value for key in ("USB Interface Name", "kUSBString")
             for value in [interface.get(key)] if isinstance(value, str) and value}
    if names:
        return next(iter(names)) if len(names) == 1 else ""
    name = interface.get("IORegistryEntryName", "")
    return name if isinstance(name, str) and name not in _USB_INTERFACE_CLASSES else ""


def _iris_functions(device: dict) -> dict[int, dict[str, Any]]:
    """Bind BSD data interfaces to this device's named CDC control descriptors."""
    interfaces: dict[int, list[dict]] = {}

    def collect(node: dict) -> None:
        for child in node.get("IORegistryEntryChildren", []):
            if not isinstance(child, dict) or child.get("IOObjectClass") in _USB_DEVICE_CLASSES:
                continue
            number = child.get("bInterfaceNumber")
            if child.get("IOObjectClass") in _USB_INTERFACE_CLASSES and isinstance(number, int):
                interfaces.setdefault(number, []).append(child)
            collect(child)

    collect(device)
    functions = {}
    for control_number, expected in _IRIS_INTERFACES.items():
        control, data = interfaces.get(control_number, []), interfaces.get(control_number + 1, [])
        if (len(control) == len(data) == 1 and control[0].get("bInterfaceClass") == 2
                and data[0].get("bInterfaceClass") == 10 and _interface_name(control[0]) == expected):
            # Firmware declares CDC pairs 0/1 and 2/3. Use the control number
            # as the stable function key, irrespective of its BSD node's suffix.
            functions[id(data[0])] = {"interface": expected, "number": control_number}
    return functions


def registry_interfaces(tree: list[dict]) -> dict[str, dict[str, Any]]:
    """Associate BSD nodes with validated CDC functions within one USB device."""
    found: dict[str, dict[str, Any]] = {}

    def visit(node: dict, device: dict, interface: dict, functions: dict) -> None:
        if node.get("IOObjectClass") in _USB_DEVICE_CLASSES:
            device, interface = node, {}
            functions = _iris_functions(node) if (node.get("idVendor"), node.get("idProduct")) == (0x303A, 0x4002) else {}
        if node.get("IOObjectClass") in _USB_INTERFACE_CLASSES:
            interface = node
        for key in ("IOCalloutDevice", "IODialinDevice"):
            path = node.get(key)
            if isinstance(path, str) and interface:
                descriptor = {"interface": _interface_name(interface), "number": interface.get("bInterfaceNumber")}
                if (device.get("idVendor"), device.get("idProduct")) == (0x303A, 0x4002):
                    descriptor = functions.get(id(interface), {"interface": "", "number": None})
                found[path.replace("/dev/tty.", "/dev/cu.")] = descriptor
        for child in node.get("IORegistryEntryChildren", []):
            if isinstance(child, dict):
                visit(child, device, interface, functions)

    for root in tree:
        if isinstance(root, dict):
            visit(root, {}, {}, {})
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
    if sys.platform == "win32":
        return _windows_ports(ports)
    if sys.platform != "darwin":
        return ports
    descriptors = _mac_interfaces() if ports else {}
    result = {}
    for original in ports:
        # Enriched copies carry a function role beyond pySerial's public fields.
        port: Any = copy.copy(original)
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


def _windows_interfaces() -> dict[str, dict[str, Any]]:
    # pySerial loses MI_00 and both CDC interface names on Windows. Read the
    # native interface devnodes by their assigned COM name, never COM order or
    # USB serial number. HELLO still verifies the protocol role and identity.
    import winreg

    found: dict[str, dict[str, Any]] = {}
    ambiguous = set()
    for number, name in _IRIS_INTERFACES.items():
        path = rf"SYSTEM\CurrentControlSet\Enum\USB\VID_303A&PID_4002&MI_{number:02X}"
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as parent:
                index = 0
                while True:
                    try:
                        instance = winreg.EnumKey(parent, index)
                    except OSError:
                        break
                    index += 1
                    try:
                        with winreg.OpenKey(parent, instance) as device:
                            try:
                                location = winreg.QueryValueEx(device, "LocationInformation")[0]
                            except OSError:
                                location = ""
                            with winreg.OpenKey(device, "Device Parameters") as parameters:
                                port = winreg.QueryValueEx(parameters, "PortName")[0]
                        if not isinstance(port, str):
                            continue
                        descriptor = {"number": number, "interface": name,
                                      "location": location if isinstance(location, str) else ""}
                        key = port.upper()
                        if key in found and found[key] != descriptor:
                            ambiguous.add(key)
                        found[key] = descriptor
                    except OSError:
                        continue
        except OSError:
            continue
    return {key: value for key, value in found.items() if key not in ambiguous}


def _windows_ports(ports: list[Any]) -> list[Any]:
    descriptors = _windows_interfaces() if ports else {}
    result = []
    for original in ports:
        descriptor = descriptors.get(original.device.upper())
        if (original.vid, original.pid) != (0x303A, 0x4002) or descriptor is None:
            result.append(original)
            continue
        port = copy.copy(original)
        number = descriptor["number"]
        port.interface = descriptor["interface"]
        port.iris_link_role = "data" if number == 2 else "control"
        physical = descriptor["location"]
        if physical:
            # Use native topology even when only one CDC has re-enumerated.
            # Borrowing the data port's pySerial location changes the console
            # key when that port appears later, creating duplicate supervisors.
            port.location = "win-" + physical + f":1.{number}"
        result.append(port)
    return result


def link_role(port: Any) -> str:
    return getattr(port, "iris_link_role", None) or (
        "data" if getattr(port, "interface", None) == "ESP-Iris 0.2 data" else "control")

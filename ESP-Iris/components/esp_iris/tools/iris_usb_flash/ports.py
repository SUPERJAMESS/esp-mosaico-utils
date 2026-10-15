# SPDX-License-Identifier: Apache-2.0
"""Follow one Iris console through the S31 ROM USB re-enumeration.

Adapted from esp-mosaico-bsp 3d0dcb5 idf_ext.py. A composite data interface is
never a programming endpoint; verify the ROM MAC before handing off to IDF.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import time
from contextlib import ExitStack
from dataclasses import dataclass

import serial
from iris_gateway.discovery import usb_endpoint
from iris_gateway.hardware_reset import execute_reset, reset_steps
from iris_gateway.link import EndpointLock
from iris_gateway.usb_ownership import usb_admission
from serial.tools import list_ports

ROM_IDS = (0x303A, 0x0020)


class UsbFlashError(RuntimeError):
    """A device could not be uniquely selected or followed through reset."""


def mac_serial(value: str) -> str:
    return re.sub(r"[^0-9a-f]", "", value.lower())


def canonical_port(value: str) -> str:
    if sys.platform == "darwin":
        value = value.replace("/dev/tty.usb", "/dev/cu.usb")
    return os.path.realpath(value) if os.name == "posix" else value.upper()


@dataclass(frozen=True)
class Port:
    device: str
    vid: int
    pid: int
    serial: str
    location: str
    interface: str

    @property
    def topology(self) -> str:
        return self.location.split(":", 1)[0]

    @property
    def rom(self) -> bool:
        return (self.vid, self.pid) == ROM_IDS

    def console(self, vid: int, pid: int) -> bool:
        # Interface strings come from esp_iris_transport_usb.c. Do not infer
        # console/data from tty numbering or from a shared VID/PID alone.
        return ((self.vid, self.pid) == (vid, pid)
                and self.interface == "ESP-Iris 0.2 console"
                and len(mac_serial(self.serial)) == 12)


def ports() -> list[Port]:
    return [Port(canonical_port(p.device), p.vid, p.pid, p.serial_number or "",
                 p.location or "", p.interface or "")
            for p in list_ports.comports() if p.vid is not None and p.pid is not None
            and not (sys.platform == "darwin" and p.device.startswith("/dev/tty."))]


def select_port(requested: str | None, vid: int, pid: int) -> Port | None:
    available = ports()
    if requested:
        matches = [p for p in available if p.device == canonical_port(requested)]
        if len(matches) != 1:
            # UART and non-USB ports retain the native IDF behavior.
            return None
        chosen = matches[0]
        if chosen.rom or chosen.console(vid, pid):
            return chosen
        if (chosen.vid, chosen.pid) == (vid, pid):
            raise UsbFlashError("Select the ESP-Iris console interface, not its data interface.")
        return None
    candidates = [p for p in available if p.console(vid, pid) or p.rom]
    if len(candidates) != 1:
        found = ", ".join(p.device for p in candidates) or "none"
        raise UsbFlashError(f"Expected one Iris console or S31 ROM port; found {found}. "
                            "Select the board with -p, or enter Boot/ROM mode.")
    return candidates[0]


def claim_device(origin: Port) -> ExitStack:
    """Keep Gateway from opening either CDC during flash and monitor handoff."""
    stack = ExitStack()
    peers = [p for p in ports() if origin.topology and p.topology == origin.topology]
    keys = {usb_endpoint({"location": p.location, "path": p.device}) for p in [origin, *peers]}
    if re.fullmatch(r".+:1\.\d+", origin.location):
        # Linux exposes interface locations even when the board currently has
        # only the ROM CDC. Reserve both future Iris interfaces before reset.
        keys.update("usb:location=" + origin.topology + suffix for suffix in (":1.0", ":1.2"))
    try:
        with usb_admission():
            for key in sorted(keys):
                lock = EndpointLock(key)
                stack.callback(lock.close)
                lock.acquire()
    except RuntimeError as error:
        stack.close()
        raise UsbFlashError("The board is in use by Gateway or another flash operation. "
                            "Release its console and data connections before native flashing.") from error
    except BaseException:
        stack.close()
        raise
    return stack


def enter_download(port: Port) -> None:
    # Initialize both lines before open, matching Iris's normal no-reset attach.
    # Only the explicit, held reset sequence below requests ROM entry.
    device = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                           write_timeout=1, exclusive=True if os.name == "posix" else None)
    try:
        device.dtr = True
        device.rts = True
        device.port = port.device
        device.open()
        execute_reset(device, reset_steps("rom", "usb_serial_jtag"))
    finally:
        device.close()


def wait_for_port(origin: Port, vid: int, pid: int, *, rom: bool,
                  expected_mac: str = "", timeout: float = 15.0) -> Port:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        candidates = [p for p in ports()
                      if (p.rom if rom else p.console(vid, pid))
                      and (not origin.topology or p.topology == origin.topology)
                      and (rom or mac_serial(p.serial) == expected_mac)]
        if len(candidates) > 1:
            raise UsbFlashError("Multiple USB endpoints match the selected device; refusing to guess.")
        if len(candidates) == 1:
            return candidates[0]
        time.sleep(0.05)
    phase = "ROM download" if rom else "application console"
    raise UsbFlashError(f"Timed out waiting for the same board's {phase} port. "
                        "Older firmware needs manual Boot/ROM entry.")


def read_rom_mac(port: Port) -> str:
    command = [sys.executable, "-m", "esptool", "--chip", "esp32s31", "--port", port.device,
               "--before", "no-reset", "--after", "no-reset", "--no-stub", "read-mac"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=15, check=True)
    # esptool prints it in both the connection banner and read-mac result.
    found = set(re.findall(r"(?im)^\s*MAC:\s*([0-9a-f]{2}(?::[0-9a-f]{2}){5})\s*$",
                           result.stdout.lower()))
    if len(found) != 1:
        raise UsbFlashError("The selected ROM port did not report one hardware MAC.")
    return mac_serial(found.pop())


def prepare_rom(origin: Port, vid: int, pid: int) -> tuple[Port, str]:
    rom = origin
    if not origin.rom:
        if not origin.topology:
            raise UsbFlashError("USB physical location is unavailable. Enter Boot/ROM mode "
                                "manually and select its port with -p.")
        print(f"Iris: requesting ROM download on {origin.device}...", flush=True)
        enter_download(origin)
        rom = wait_for_port(origin, vid, pid, rom=True)
    mac = read_rom_mac(rom)
    if not origin.rom and mac != mac_serial(origin.serial):
        raise UsbFlashError("ROM hardware MAC differs from the selected Iris device; no flash performed.")
    return rom, mac

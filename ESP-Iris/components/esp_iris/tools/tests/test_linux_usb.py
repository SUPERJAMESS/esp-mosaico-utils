from __future__ import annotations

import asyncio
import posixpath
import sys
from types import SimpleNamespace
from unittest.mock import patch

import pytest
from test_usb_ownership import until
from test_usb_reconnect_policy import board, open_board

from iris_gateway import usb_descriptors
from iris_gateway.hub import IrisHub
from iris_gateway.link import SerialLink


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="Linux sysfs parser")
@pytest.mark.parametrize("attribute", ["bNumInterfaces", "idVendor", "idProduct"])
def test_disappearing_sysfs_attribute_is_retryable(attribute, monkeypatch):
    from serial.tools import list_ports_linux

    def realpath(path):
        if path.endswith("/subsystem"):
            return "/sys/bus/usb"
        if path.endswith("/device"):
            return "/sys/devices/test/1-2/1-2:1.0"
        return path

    def read_attribute(self, path, name):
        if name == attribute:
            return None
        return {"bNumInterfaces": "2", "idVendor": "303a", "idProduct": "4002"}.get(name, "")

    fake_os = SimpleNamespace(path=SimpleNamespace(
        islink=lambda path: False, exists=lambda path: True, realpath=realpath,
        basename=posixpath.basename, dirname=posixpath.dirname, join=posixpath.join,
    ))
    monkeypatch.setattr(list_ports_linux, "os", fake_os)
    monkeypatch.setattr(list_ports_linux.SysFS, "read_line", read_attribute)
    monkeypatch.setattr(usb_descriptors.list_ports, "comports",
                        lambda: [list_ports_linux.SysFS("/dev/ttyACM-test")])
    with pytest.raises(OSError, match="USB enumeration.*TypeError") as error:
        usb_descriptors.serial_ports()
    assert isinstance(error.value.__cause__, TypeError)


@pytest.mark.parametrize("platform,wrapped", [("linux", True), ("darwin", False), ("win32", False)])
def test_enumeration_conversion_guard_is_linux_only(platform, wrapped, monkeypatch):
    monkeypatch.setattr(usb_descriptors, "sys", SimpleNamespace(platform=platform))
    monkeypatch.setattr(usb_descriptors.list_ports, "comports", lambda: int("partial descriptor"))
    with pytest.raises(OSError if wrapped else ValueError):
        usb_descriptors.serial_ports()


@pytest.mark.parametrize("mode", ["pending", "discovery", "reconnect"])
def test_enumeration_race_recovers_without_reclaim(mode, tmp_path, monkeypatch):
    monkeypatch.setattr("iris_gateway.link.tempfile.gettempdir", lambda: str(tmp_path))
    monkeypatch.setattr(usb_descriptors, "sys", SimpleNamespace(platform="linux"))
    unavailable = True
    current = board()

    def enumerate_ports():
        if unavailable:
            raise TypeError("int() argument must not be NoneType")
        return [current]

    monkeypatch.setattr(usb_descriptors.list_ports, "comports", enumerate_ports)

    async def scenario():
        nonlocal unavailable
        hub = IrisHub(reconnect_min_seconds=0.005, reconnect_max_seconds=0.01)
        links = []

        async def open_link(path, **kwargs):
            link = await open_board(path, **kwargs)
            links.append(link)
            return link

        try:
            with patch.object(SerialLink, "open", side_effect=open_link):
                if mode == "discovery":
                    await hub.start_usb_discovery(0.005)
                    await asyncio.sleep(0.02)
                    assert not hub._discovery_task.done()
                elif mode == "pending":
                    await hub.add_usb(current.device)
                    await until(lambda: hub.list_endpoints()[0]["attempt"] >= 2)
                else:
                    unavailable = False
                    await hub.add_usb(current.device)
                    await until(lambda: bool(hub.list_devices()))
                    unavailable = True
                    await links[0].incoming.put(b"")
                    await until(lambda: "USB enumeration" in (hub.list_endpoints()[0]["error"] or ""))
                    assert not hub.list_devices()
                unavailable = False
                await until(lambda: bool(hub.list_devices()))
                assert hub.list_endpoints()[0]["state"] == "ready"
                assert len(links) == (2 if mode == "reconnect" else 1)
        finally:
            await hub.close()
        assert all(link.closed for link in links)

    asyncio.run(scenario())

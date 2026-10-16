from __future__ import annotations

import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from iris_gateway.hardware_reset import execute_reset, reset_steps
from iris_gateway.hub import IrisHub
from iris_gateway.link import SerialLink


class Pins:
    def __init__(self):
        self.events = []
        self._dtr = False

    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, value):
        self._dtr = value
        self.events.append(("D", value))

    @property
    def rts(self):
        raise AssertionError("reset does not read RTS")

    @rts.setter
    def rts(self, value):
        self.events.append(("R", value))

    @property
    def cts(self):
        raise AssertionError("CTS cannot reset a device")


@pytest.mark.parametrize("circuit", ["uart", "usb_serial_jtag"])
def test_attach_never_toggles_or_discards_input(circuit):
    port = Pins()
    execute_reset(port, reset_steps("attach", circuit))
    assert port.events == []


def test_serial_jtag_rom_sequence_preserves_monitor_timing():
    port = Pins()
    execute_reset(port, reset_steps("rom", "usb_serial_jtag"),
                  sleep=lambda value: port.events.append(("W", value)))
    assert [item for item in port.events if item[0] == "W"] == [("W", 0.1)] * 3
    assert [item[1] for item in port.events if item[0] == "R"] == [False, False, True, True, False]
    assert port.dtr is False


@pytest.mark.parametrize("steps", [[("CTS", 1)], [("W", 20)], [("D", 2)], [("R", 1)] * 33])
def test_invalid_sequence_is_rejected_before_first_pin_change(steps):
    port = Pins()
    with pytest.raises(ValueError):
        execute_reset(port, steps)
    assert port.events == []


def test_serial_reset_waits_for_reader_and_keeps_connection_open():
    async def scenario():
        port = Pins()
        port.is_open = True
        link = SerialLink("fake", port)
        task = asyncio.create_task(link.hardware_reset("run", "uart"))
        await asyncio.sleep(0.01)
        assert port.events == []
        link.reader_started.set()
        await task
        assert port.is_open
        assert [value for action, value in port.events if action == "R"] == [True, False]
    asyncio.run(scenario())


def test_console_only_device_can_reset_without_iris_identity():
    async def scenario():
        hub = IrisHub()
        link = SerialLink("fake", Pins(), endpoint="usb:fake")
        link.hardware_reset = AsyncMock()
        hub._endpoint_sessions[link.endpoint] = SimpleNamespace(
            link=link, _console=True, info=None, capture_id="raw-boot")
        hub._endpoint_states[link.endpoint] = {"vid": 0x303A, "pid": 0x1001}
        result = await hub.hardware_reset(link.endpoint, "rom")
        link.hardware_reset.assert_awaited_once_with("rom", "usb_serial_jtag", chip=None)
        assert result["reader_started_before_reset"] is True
        assert result["capture_id"] == "raw-boot" and result["device_id"] is None
    asyncio.run(scenario())


def test_reset_identity_uses_only_the_current_session():
    hub = IrisHub()
    endpoint = "usb:console"
    hub._endpoint_states[endpoint] = {"device_id": "remembered-device"}
    assert hub.device_for_endpoint(endpoint) is None
    session = SimpleNamespace(info=None)
    hub._endpoint_sessions[endpoint] = session
    assert hub.device_for_endpoint(endpoint) is None
    session.info = SimpleNamespace(device_id="live-device")
    assert hub.device_for_endpoint(endpoint) == "live-device"
    del hub._endpoint_sessions[endpoint]
    assert hub.device_for_endpoint(endpoint) is None


@pytest.mark.parametrize("chip", [None, "", "unknown"])
def test_uart_rom_requires_known_target(chip):
    with pytest.raises(ValueError, match="chip target"):
        reset_steps("rom", "uart", chip=chip)


def test_esp32_rom_uses_longer_boot_timing():
    assert ("W", 1.3) in reset_steps("rom", "uart", chip="esp32")
    assert ("W", 0.1) in reset_steps("rom", "uart", chip="esp32c2")


def test_hub_passes_live_target_to_reset():
    async def scenario():
        hub = IrisHub()
        link = SerialLink("fake", Pins(), endpoint="serial:fake")
        link.hardware_reset = AsyncMock()
        hub._endpoint_sessions[link.endpoint] = SimpleNamespace(
            link=link, _console=True, info=SimpleNamespace(device_id="device", chip_target="esp32"),
            capture_id="capture")
        hub._endpoint_states[link.endpoint] = {}
        await hub.hardware_reset(link.endpoint, "rom")
        link.hardware_reset.assert_awaited_once_with("rom", "uart", chip="esp32")
    asyncio.run(scenario())

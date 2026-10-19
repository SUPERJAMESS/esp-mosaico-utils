"""Passive connection discovery and live-versus-historical presentation."""
from types import SimpleNamespace
from unittest.mock import patch

import pytest
from test_host_operations import HostHub

from iris_gateway.device_state import describe
from iris_gateway.discovery import discover_iris_usb_devices
from iris_gateway.gateway import GatewayService
from iris_gateway.store import GatewayStore


def port(path, vid, pid, product, location, interface=""):
    return SimpleNamespace(device=path, vid=vid, pid=pid, product=product,
                           serial_number="same", location=location, interface=interface)


def test_picker_lists_mixed_transports_without_widening_auto_discovery(tmp_path, monkeypatch):
    ports = [port("/dev/ttyACM0", 0x303A, 0x4002, "ESP-Iris", "1-2:1.0", "ESP-Iris 0.2 console"),
             port("/dev/ttyACM2", 0x303A, 0x4002, "ESP-Iris", "1-2:1.2", "ESP-Iris 0.2 data"),
             port("/dev/ttyACM1", 0x303A, 0x1001, "USB JTAG/serial debug unit", "1-8:1.0"),
             port("/dev/ttyUSB0", 0x10C4, 0xEA60, "CP2102N", "1-7")]
    monkeypatch.setattr("iris_gateway.discovery.serial_ports", lambda: ports)
    store = GatewayStore(tmp_path)
    service = GatewayService(store, instance_id="picker")
    hub = HostHub()
    hub.list_endpoints = list
    service.attach_hub(hub)
    try:
        with patch("serial.Serial") as serial_open:
            endpoints = service.list_endpoints()
            serial_open.assert_not_called()
        assert len(endpoints) == 4
        assert {item["state"] for item in endpoints} == {"discovered"}
        assert all(item["present"] and not item.get("device_id") for item in endpoints)
        uart = next(item for item in endpoints if item["uart"])
        assert uart["device_path"] == "/dev/ttyUSB0"
        assert next(item for item in endpoints if item["link_role"] == "data")["endpoint"] == "usb:location=1-2:1.2"
        assert len(discover_iris_usb_devices()) == 2
        assert not hub.actions
    finally:
        store.close()


@pytest.mark.parametrize("attempt_state,expected", [("discovered", "discovered"), ("connecting", "connecting"),
                                                    ("handshaking", "connecting"), ("backoff", "discovered")])
def test_present_port_only_connecting_during_active_attempt(tmp_path, attempt_state, expected):
    store = GatewayStore(tmp_path)
    service = GatewayService(store, instance_id="state")
    hub = HostHub()
    endpoint = "usb:location=test"
    hub.list_endpoints = lambda: [{"endpoint": endpoint, "state": attempt_state}]
    service.attach_hub(hub)
    try:
        with patch("iris_gateway.device_state.resolve_usb_port", return_value={"vid": 0x303A, "pid": 0x4002}):
            assert describe(service, {"endpoint": endpoint, "connected": False}, workers=[])["state"] == expected
    finally:
        store.close()


def test_disconnected_cache_never_reports_live_links(tmp_path, monkeypatch):
    monkeypatch.setattr("iris_gateway.discovery.serial_ports", list)
    store = GatewayStore(tmp_path)
    service = GatewayService(store, instance_id="cache")
    hub = HostHub()
    hub.list_devices = list
    service.attach_hub(hub)
    store.remember_device({"device_id": "history", "endpoint": "usb:location=gone", "connected": True,
                           "present": True, "control_link": {"session_id": 1}, "data_link": {"session_id": 2}, "data_available": True})
    try:
        item, = service.list_devices()
        assert item["cached"] and item["stale"]
        assert not item["connected"] and not item["online"] and not item["data_available"]
        assert item["control_link"] is None and item["data_link"] is None
        assert item["state"] == "offline"
    finally:
        store.close()

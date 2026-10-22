from __future__ import annotations

import asyncio
import dataclasses
from types import SimpleNamespace

from iris_gateway.device_info import DeviceInfo
from iris_gateway.gateway import GatewayService
from iris_gateway.hub import IrisHub
from iris_gateway.store import GatewayStore


def nine_device_hub():
    hub = IrisHub()
    for number in range(9):
        info = DeviceInfo(
            device_id=f"device-{number}", boot_id=2**63 + number, session_id=number,
            endpoint=f"usb:{number}", transport=1, project_name="test", app_version="old",
            idf_version="6.2.0", firmware_sha256="a" * 64, reset_reason=1,
            capabilities=0xffff, auth_mode=0, max_payload=4000,
        )
        control = SimpleNamespace(info=info, link=SimpleNamespace(endpoint=info.endpoint),
                                  console_available=True)
        data = SimpleNamespace(info=dataclasses.replace(info, endpoint=f"usb:{number}-data",
                                                       link_role="data"))
        hub._devices[info.device_id] = control
        hub._links[info.device_id] = {"control": control, "data": data}
    return hub


def test_one_device_snapshot_does_not_serialize_other_devices(monkeypatch):
    hub = nine_device_hub()
    expected = hub.list_devices()[4]
    serialized = []
    original = DeviceInfo.as_dict

    def observe(info):
        serialized.append(info.device_id)
        return original(info)

    monkeypatch.setattr(DeviceInfo, "as_dict", observe)
    assert hub.device_snapshot("device-4") == expected
    assert serialized == ["device-4"] * 3
    assert hub.device_snapshot("absent") is None
    assert serialized == ["device-4"] * 3


def test_snapshot_reflects_current_boot_and_data_link():
    hub = nine_device_hub()
    first = hub.device_snapshot("device-4")
    hub._devices["device-4"].info.boot_id = 2**63 + 42
    hub._devices["device-4"].info.app_version = "new"
    del hub._links["device-4"]["data"]
    current = hub.device_snapshot("device-4")
    assert current["boot_id_text"] == str(2**63 + 42)
    assert current["app_version"] == "new"
    assert current["data_link"] is None and current["data_available"] is False
    assert first["app_version"] == "old" and first["data_available"] is True
    del hub._devices["device-4"]
    assert hub.device_snapshot("device-4") is None


def test_event_ingestion_avoids_fleet_snapshot_and_keeps_latest_identity(tmp_path, monkeypatch):
    async def scenario():
        hub = nine_device_hub()
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="snapshot-regression")
        service.attach_hub(hub)

        def fleet_snapshot():
            raise AssertionError("per-event processing must not serialize the entire fleet")

        monkeypatch.setattr(hub, "list_devices", fleet_snapshot)
        try:
            for version in ("first", "latest"):
                hub._devices["device-4"].info.app_version = version
                await service.on_device_event({"kind": "log", "device_id": "device-4",
                                               "text": version})
            await service.flush_events()
            cached = store.cached_devices()
            assert len(cached) == 1 and cached[0]["app_version"] == "latest"
            assert cached[0]["boot_id_text"] == str(2**63 + 4)
            assert service._event_writer.snapshot()["dropped_events"] == 0
        finally:
            await service.close_events()
            store.close()

    asyncio.run(scenario())

"""ROM acceptance follows hardware identity and can resume without a writer."""
import asyncio
import json
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

import pytest
from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.host_operations import HostOperations, _target_identity
from iris_gateway.operations import OperationOutcomeUnknown
from iris_gateway.reconciliation import reconciliation_records
from iris_gateway.security import Actor
from iris_gateway.store import GatewayStore

MAC = "30:ed:a0:f4:51:8e"
DEVICE = _target_identity(None, MAC)
IMAGE = {"project_name": "vibe-mode", "elf_sha256": "ab" * 32, "sha256": "cd" * 32}
STATUS = {"device_id": DEVICE, "hardware_mac": MAC, "boot_id": 2, "firmware_mode": "recovery",
          "app_version": "new", "project_name": "vibe-mode", "firmware_sha256": "ab" * 32,
          "capability_names": ["ota"], "endpoint": "usb:location=new-interface"}
PARAMS = {"expected_device_id": DEVICE, "expected_hardware_mac": MAC,
          "expected_version": "new", "expected_image": IMAGE, "endpoints": [{"endpoint": "usb:location=old-rom"}]}


@pytest.mark.parametrize("change", [None, {"device_id": "other"}, {"hardware_mac": "00:00:00:00:00:00"},
                                   {"boot_id": 1}, {"firmware_sha256": "00" * 32}, {"capability_names": []}])
def test_acceptance_uses_identity_not_endpoint(change):
    async def scenario():
        status = {**STATUS, **(change or {})}
        hub = SimpleNamespace(list_devices=lambda: [status], status=AsyncMock(return_value=status))
        verifier = HostOperations(SimpleNamespace(device_hub=hub, project=None))
        if change:
            with pytest.raises(OperationOutcomeUnknown):
                await verifier.verify(DEVICE, {"boot_id": 1}, PARAMS, .01)
        else:
            assert await verifier.verify(DEVICE, {"boot_id": 1}, PARAMS, .01) == status
    asyncio.run(scenario())


def test_recovery_reconnect_uses_existing_identity_admission(tmp_path):
    from test_single_device_selection import D, gateway, usb

    async def scenario():
        async with gateway(tmp_path) as g:
            g.registry.acquire("usb:location=old-rom", {})
            g.registry.bind("usb:location=old-rom", D)
            status = {**STATUS, "device_id": D, "endpoint": "usb:location=changed:1.0"}
            with patch("iris_gateway.device_selection.discover_iris_usb_devices", return_value=[usb("changed:1.0")]), \
                    patch.object(g.hub, "status", AsyncMock(return_value=status)):
                verified = await g.project.service.host_operations.verify(D, {"boot_id": 1}, PARAMS, .5)
            assert verified == status
            assert g.connect.call_args.args[0] == "usb:location=changed:1.0"
            assert g.registry.claim("device:" + D)["owner"] == "a"
            # A later application enumeration can replace the endpoint again;
            # the same admission/ownership contract still follows the chip.
            g.live.clear()
            with patch("iris_gateway.device_selection.discover_iris_usb_devices", return_value=[usb("application:1.0")]):
                application = await g.project.acquire({"device_id": D, "timeout": .5})
            assert application["device_id"] == D
            assert application["endpoint"] == "usb:location=application:1.0"
            assert {claim["device_id"] for claim in g.registry.claims()} == {D}
    asyncio.run(scenario())


@pytest.mark.parametrize("write_result", [0, 1, None])
@pytest.mark.parametrize("original_status", ["outcome_unknown", "failed"])
def test_reconcile_survives_restart_and_never_replays(tmp_path, write_result, original_status):
    async def scenario():
        store = GatewayStore(tmp_path)
        store.create_operation({"operation_id": "op", "device_id": "endpoint::usb:location=old-rom",
                                "actor_type": "agent", "actor_name": "test", "action": "host.recovery",
                                "params": PARAMS, "status": original_status, "created_ns": 1})
        original = store.operation("op")
        folder = store.artifacts_dir / "host-operations/op"
        folder.mkdir(parents=True)
        (folder / "before.json").write_text(json.dumps({"device_before": {"boot_id": 1}}))
        if write_result is not None:
            (folder / "result.json").write_text(json.dumps({"returncode": write_result}))
        store.close()
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="restarted")
        # No flash, reset or subprocess interface exists on this test hub.
        service.hub = SimpleNamespace(list_devices=lambda: [STATUS], status=AsyncMock(return_value=STATUS))
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            with patch("iris_gateway.host_operations.asyncio.create_subprocess_exec", side_effect=AssertionError("write replay")):
                for _ in range(2):
                    response = await client.post("/v2/operations/op/reconcile")
                    assert response.status == 201, await response.text()
                    record = await response.json()
                    assert record["outcome"] == ("observed_success" if write_result == 0 else "outcome_unknown")
                    assert record["replayed"] is False
            assert store.operation("op") == original
            assert len(reconciliation_records(store, "op")) == 2
        finally:
            await client.close()
            store.close()
    asyncio.run(scenario())


def test_active_writer_blocks_reconciliation(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        store.create_operation({"operation_id": "op", "device_id": DEVICE, "actor_type": "agent",
                                "actor_name": "test", "action": "host.recovery", "params": PARAMS,
                                "status": "outcome_unknown", "created_ns": 1})
        service = GatewayService(store, instance_id="test")
        with patch("iris_gateway.host_operations.active_workers", return_value=[{"resources": [DEVICE]}]), \
                pytest.raises(RuntimeError, match="writer still owns"):
            await service.host_operations.reconcile("op", Actor("agent", "test"))
        assert reconciliation_records(store, "op") == []
        store.close()
    asyncio.run(scenario())

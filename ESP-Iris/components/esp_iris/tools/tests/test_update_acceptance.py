"""Exercise final contract validation through both update workflows."""
import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

import pytest
from test_update_compatibility import CONTRACT

from iris_gateway.gateway import GatewayService
from iris_gateway.system_update import SystemUpdateComponentKind

META = {"chip_id": 0x20, "project_name": "game", "version": "1.0",
        "sha256": "ab" * 32, "elf_sha256": "cd" * 32}


class UpdateHub:
    def __init__(self, final_changes, fast):
        self.current = {**CONTRACT, "device_id": "device", "boot_id": 1,
                        "firmware_mode": "recovery", "app_version": "0.1"}
        self.final_changes, self.fast = final_changes, fast
        self.writes = 0
        self.transitions = 0
        self.queue = asyncio.Queue()
        self.operation_id = ""
        self.subscribe = Mock(return_value=self.queue)
        self.unsubscribe = Mock()

    async def status(self, device_id):
        return dict(self.current)

    async def enter_recovery(self, device_id):
        self.transitions += 1
        self.current.update(firmware_mode="recovery", boot_id=self.current["boot_id"] + 1)

    async def write(self):
        self.writes += 1
        self.current.update(firmware_mode="normal", boot_id=self.current["boot_id"] + 1,
                            project_name="game", app_version="1.0", firmware_sha256=META["elf_sha256"])
        self.current.update(self.final_changes)
        await self.queue.put({"event_name": "healthy", "boot_id": self.current["boot_id"]})
        return {"healthy": self.fast, "boot_id": self.current["boot_id"], "completion_evidence": "session_close"}

    async def ota_update(self, *args, **kwargs):
        return await self.write()

    async def system_update(self, *args, operation_id, **kwargs):
        self.operation_id = operation_id.hex()
        return await self.write()

    async def system_update_inventory(self, device_id):
        return {"partition_table_sha256": "ef" * 32,
                "last_operation_id": self.operation_id, "last_result": 0}


async def update(hub, method):
    service = GatewayService.__new__(GatewayService)
    service.hub, service.operations = hub, AsyncMock()
    service.preserve_coredump = AsyncMock(return_value=None)
    if method == "ota":
        return await service.closed_loop_ota("device", b"image", META, "op", compatibility=CONTRACT)
    bundle = SimpleNamespace(chip_id=0x20, target_layout_sha256="ef" * 32,
                             components=[SimpleNamespace(kind=SystemUpdateComponentKind.APPLICATION, data=b"image")])
    with patch("iris_gateway.system_update_workflow.inspect_firmware_image",
               return_value=SimpleNamespace(as_dict=lambda: META)):
        return await service.closed_loop_system_update("device", bundle, "op", CONTRACT)


@pytest.mark.parametrize("method,fast", [("ota", True), ("ota", False), ("system", False)])
@pytest.mark.parametrize("changes", [
    {"firmware_mode": "unknown"}, {"firmware_mode": "recovery"},
    {"product_contract": ""}, {"board_id": "wrong"}, {"layout_id": "wrong"},
    {"recovery_abi": 0}, {"device_id": "other"},
    {"crash_count": 1}, {"crash_loop_triggered": True},
    {"crash_recovery_pending": True}, {"crash_state_error": 1},
])
def test_healthy_matching_image_cannot_hide_a_bad_final_contract(method, fast, changes):
    async def scenario():
        hub = UpdateHub(changes, fast)
        with pytest.raises(ValueError):
            await update(hub, method)
        assert hub.writes == 1
        hub.unsubscribe.assert_called_once()
    asyncio.run(scenario())


@pytest.mark.parametrize("method,fast", [("ota", True), ("ota", False), ("system", False)])
def test_first_install_then_normal_recovery_normal_update(method, fast):
    async def scenario():
        hub = UpdateHub({}, fast)
        assert (await update(hub, method))["healthy"]
        assert hub.current["boot_id"] == 2
        assert (await update(hub, method))["healthy"]
        assert hub.current["boot_id"] == 4
        assert hub.transitions == 1
        assert hub.writes == 2
    asyncio.run(scenario())

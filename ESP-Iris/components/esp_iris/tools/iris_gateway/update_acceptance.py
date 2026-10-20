"""Final firmware contract acceptance shared by app and system updates."""
from __future__ import annotations

from typing import Any

from .compatibility import validate_update_compatibility


def validate_crash_state(status: dict[str, Any]) -> None:
    if (status.get("crash_count", 0) or status.get("crash_loop_triggered")
            or status.get("crash_recovery_pending") or status.get("crash_state_error", 0)):
        raise ValueError("post-update device reports a crash or unavailable crash state")


def validate_updated_contract(status: dict[str, Any], device_id: str, chip_id: Any,
                              expectation: dict[str, Any], *, role: str = "normal") -> None:
    if status.get("device_id") != device_id:
        raise ValueError("post-update Device ID does not match the selected device")
    if status.get("firmware_mode") != role:
        raise ValueError(f"post-update firmware role must be {role}; got {status.get('firmware_mode')!r}")
    validate_update_compatibility(status, chip_id, expectation, recovery=role == "recovery")
    validate_crash_state(status)

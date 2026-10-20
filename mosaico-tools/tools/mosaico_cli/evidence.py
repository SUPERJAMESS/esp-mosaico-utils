"""Collect device evidence through the project's existing Iris CLI session."""
from __future__ import annotations

from typing import Any

from .errors import DeviceError, OperationError, OutcomeUnknownError
from .gateway import connected_devices, ensure_gateway, gateway_json, select_device, system_inventory
from .runtime import RunContext


def _live_status(context: RunContext, session: Any, device_id: str) -> dict[str, Any]:
    response = gateway_json(context, session, "status", device_id)
    device = response.get("device", response) if isinstance(response, dict) else None
    if (not isinstance(device, dict) or device.get("device_id") != device_id
            or device.get("stale") is not False or device.get("boot_id") is None):
        raise DeviceError(
            "ESP-Iris did not return live identity and Boot ID for the selected device.",
            details={"device_id": device_id, "response": response},
        )
    return device


def collect_evidence(arguments: Any, context: RunContext) -> dict[str, Any]:
    command = arguments.command
    session = ensure_gateway(
        context, arguments.gateway_profile, select=command not in {"operation-status", "operation-reconcile"}
    )
    result = {
        "command": command,
        "gateway_started": session.started_local,
        "gateway_profile": session.profile,
        "log": str(context.log_path),
    }
    if command == "operation-status":
        response = gateway_json(context, session, "operation-status", arguments.operation_id)
        operation = response.get("operation", response) if isinstance(response, dict) else None
        if not isinstance(operation, dict) or operation.get("operation_id") != arguments.operation_id:
            raise OperationError("ESP-Iris returned an invalid operation record.")
        # A successful query does not make a failed/unknown device operation succeed.
        return {**result, "operation": operation}

    if command == "operation-reconcile":
        response = gateway_json(context, session, command, arguments.operation_id, timeout=45)
        if (not isinstance(response, dict) or response.get("operation_id") != arguments.operation_id
                or response.get("replayed") is not False):
            raise OperationError("ESP-Iris returned an invalid verification record.")
        if response.get("outcome") != "observed_success":
            error_type = OutcomeUnknownError if response.get("outcome") == "outcome_unknown" else OperationError
            raise error_type(response.get("reason") or "Operation acceptance remains unverified.",
                             details={"reconciliation": response})
        status = response.get("evidence", {}).get("status", {})
        if status.get("firmware_mode") == "recovery" and status.get("device_id") and status.get("app_version"):
            from .recovery import record_recovery_verification
            record_recovery_verification(status["device_id"], status["app_version"], status.get("boot_id"))
        return {**result, "reconciliation": response}

    selected = select_device(connected_devices(context, session), arguments.device_id)
    device_id = str(selected["device_id"])
    before = _live_status(context, session, device_id)
    result.update(device_id=device_id, boot_id=before["boot_id"], device=before)
    if command == "device-status":
        if getattr(arguments, "inventory", False):
            inventory = system_inventory(context, session, device_id)
            after = _live_status(context, session, device_id)
            if after["boot_id"] != before["boot_id"]:
                raise DeviceError("The device rebooted while reading system inventory.",
                                  details={"before": before, "after": after})
            result.update(device=after, inventory=inventory)
        return result

    if command == "restart":
        # Send exactly once. The Gateway owns restart/reconnect operation tracking.
        response = gateway_json(context, session, "restart", device_id, timeout=45)
        operation = response.get("operation", {}) if isinstance(response, dict) else {}
        restart = response.get("restart", {}) if isinstance(response, dict) else {}
        if (operation.get("device_id") != device_id or not operation.get("operation_id")
                or operation.get("status") != "succeeded" or not isinstance(restart, dict)
                or restart.get("reconnected") is not True):
            raise OperationError("Device restart was not verified.", details={"response": response})
        after = _live_status(context, session, device_id)
        if (after["boot_id"] == before["boot_id"]
                or restart.get("previous_boot_id") != before["boot_id"]
                or restart.get("boot_id") != after["boot_id"]):
            raise DeviceError("Restart Boot ID evidence does not match.",
                              details={"before": before, "after": after, "response": response})
        return {**result, "previous_boot_id": before["boot_id"],
                "boot_id": after["boot_id"], "device": after,
                "operation": operation, "restart": restart}

    output = arguments.output.expanduser().resolve()
    capture = gateway_json(context, session, "screenshot", device_id, str(output),
                           "--path", arguments.path, timeout=60)
    if (not isinstance(capture, dict) or capture.get("device_id") != device_id
            or capture.get("path") != str(output) or not capture.get("operation_id")
            or capture.get("content_type") not in {"image/png", "image/jpeg"}):
        raise OperationError("ESP-Iris returned invalid screenshot evidence.", details={"response": capture})
    try:
        size = output.stat().st_size
    except OSError as error:
        raise OperationError("The device screenshot file is unavailable.", details={"path": str(output)}) from error
    if size <= 0 or size != capture.get("bytes"):
        raise OperationError("The device screenshot file is incomplete.", details={"path": str(output)})
    after = _live_status(context, session, device_id)
    if after["boot_id"] != before["boot_id"]:
        raise DeviceError(
            "The device rebooted during capture; this image cannot verify the current boot.",
            details={"before": before, "after": after, "capture": capture},
        )
    return {**result, "device": after, "capture": capture}

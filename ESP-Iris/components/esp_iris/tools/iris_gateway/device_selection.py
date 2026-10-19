"""Bounded HELLO admission of one Device ID across transport candidates."""
from __future__ import annotations

import asyncio
import time
from typing import TYPE_CHECKING, Any

from .compat import to_thread
from .discovery import discover_iris_usb_devices, iris_usb_allowed, usb_endpoint
from .ownership import OwnershipConflict

if TYPE_CHECKING:
    from .project_gateway import ProjectGateway


async def identity_candidates(project: ProjectGateway, device_id: str) -> list[dict[str, Any]]:
    candidates = {item["endpoint"]: dict(item) for item in project.registry.known_endpoints(device_id)}
    for item in project.hub.list_endpoints():
        if (item.get("advertised_device_id") == device_id
                or (item.get("ownership") or {}).get("device_id") == device_id
                or item["endpoint"] in candidates):
            candidates[item["endpoint"]] = dict(item)
    # USB descriptors do not carry a trustworthy Device ID. An unowned Iris
    # endpoint may be probed, but only a matching HELLO may become a device.
    live_usb = set()
    for device in await to_thread(discover_iris_usb_devices):
        metadata = {key: getattr(device, key) for key in
                    ("path", "vid", "pid", "product", "serial_number", "location", "interface", "link_role")}
        metadata["device_path"] = device.device
        if not iris_usb_allowed(metadata):
            continue
        endpoint = usb_endpoint(metadata)
        live_usb.add(endpoint)
        candidates[endpoint] = {**candidates.get(endpoint, {}), **metadata,
                                "endpoint": endpoint, "present": True}
    # Never reopen stale USB paths; enumeration supplies current descriptors.
    candidates = {key: value for key, value in candidates.items()
                  if not key.startswith("usb:") or key in live_usb}
    return sorted(candidates.values(), key=lambda item: (
        not item["endpoint"].startswith("usb:"), not item.get("present", False), item["endpoint"]))


async def admit_candidates(project: ProjectGateway, candidates: list[dict[str, Any]], device_id: str | None,
                           token: str | None, timeout: float) -> dict[str, Any]:
    registry, hub = project.registry, project.hub
    failures = []
    deadline = time.monotonic() + min(30.0, max(0.1, timeout))
    for index, candidate in enumerate(candidates):
        endpoint = str(candidate["endpoint"])
        metadata = {key: value for key, value in candidate.items() if key != "ownership"}
        prior = registry.claim(endpoint)
        device_existed = bool(device_id and registry.claim("device:" + device_id))
        claim = None
        accepted = False
        try:
            if prior:
                registry._require(endpoint, ("owned",))
                if device_id and prior["device_id"] and prior["device_id"] != device_id:
                    raise OwnershipConflict("endpoint is reserved for another Device ID")
            else:
                metadata["identity_probe"] = True
            claim = registry.acquire(endpoint, metadata)
            if device_id:
                registry.bind(endpoint, device_id, verified=False)
            remaining = max(0.001, deadline - time.monotonic())
            attempt_deadline = time.monotonic() + remaining / (len(candidates) - index)
            attempt_started_ns = time.monotonic_ns()
            await asyncio.wait_for(
                hub.connect_owned(endpoint, metadata, pairing_token=token or project.tokens.get(endpoint, project.pairing_token)),
                max(0.001, attempt_deadline - time.monotonic()),
            )
            while time.monotonic() < attempt_deadline:
                for item in hub.list_devices():
                    if item.get("endpoint") == endpoint and (not device_id or item["device_id"] == device_id):
                        accepted = True
                        if token is not None:
                            project.tokens[endpoint] = token
                        return item
                state: dict[str, Any] = next((item for item in hub.list_endpoints() if item["endpoint"] == endpoint), {})
                if state.get("error") and state.get("updated_monotonic_ns", 0) >= attempt_started_ns:
                    raise ConnectionError(str(state["error"]))
                await asyncio.sleep(0.05)
            raise TimeoutError("HELLO did not validate the selected identity before the deadline")
        except (RuntimeError, ValueError, OSError, LookupError, asyncio.TimeoutError) as error:
            if len(candidates) == 1 and isinstance(error, OwnershipConflict) and claim is None:
                raise
            failures.append({"endpoint": endpoint, "error": str(error)})
        finally:
            if claim is not None and prior is None and not accepted:
                await hub.disconnect_owned_endpoint(endpoint)
                registry.release_attempt(endpoint, claim["generation"], remove_device=not device_existed)
        if time.monotonic() >= deadline:
            break
    project.selection_error("No endpoint validated the selected Device ID; inspect candidate failures.",
                            failures)

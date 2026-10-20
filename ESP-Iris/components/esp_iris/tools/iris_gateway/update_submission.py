"""Bounded retries of updates rejected before Gateway admission."""
from __future__ import annotations

import asyncio
import json
import math
from typing import Any


class GatewayHTTPError(RuntimeError):
    def __init__(self, status: int, error: Any, retry_after: str | None = None):
        super().__init__(f"HTTP {status}: {json.dumps(error, ensure_ascii=False)}")
        self.status, self.error, self.retry_after = status, error, retry_after
        self.operation_id: str | None = None

    @property
    def admission_rejected(self) -> bool:
        return (self.status == 429 and isinstance(self.error, dict)
                and self.error.get("code") == "resource_limit"
                and isinstance(self.error.get("details"), dict)
                and self.error.get("details", {}).get("resource") in {"requests", "large_requests"})

    def as_dict(self) -> dict[str, Any]:
        result = {"error": self.error, "http_status": self.status}
        if self.operation_id:
            result["operation_id"] = self.operation_id
            if self.admission_rejected:
                result.update(submission_state="rejected", write_started=False)
        return result


async def submit_system_update(session: Any, url: str, data: bytes,
                               headers: dict[str, str], ssl_value: Any,
                               decode: Any, *, admission_timeout: float = 30.0) -> Any:
    """Reuse one intent ID; never replay an admitted or uncertain write.

    Only the request-budget middleware's explicit 429 is retryable: that response
    precedes the update handler and operation creation. Network errors, 5xx and
    operation-capacity errors are deliberately left to the caller.
    """
    loop = asyncio.get_running_loop()
    deadline = loop.time() + admission_timeout
    while True:
        try:
            async with session.post(url, data=data, headers=headers, ssl=ssl_value) as response:
                return await decode(response)
        except GatewayHTTPError as error:
            error.operation_id = headers["X-Operation-ID"]
            if not error.admission_rejected:
                raise
            try:
                delay = float(error.retry_after or "1")
            except (TypeError, ValueError):
                delay = 1.0
            if not math.isfinite(delay) or delay < 0:
                delay = 1.0
            delay = max(0.05, delay)
            if loop.time() + delay >= deadline:
                raise
            await asyncio.sleep(delay)

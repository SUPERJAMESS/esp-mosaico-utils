"""Do not classify an explicitly rejected upload as an uncertain device write."""
import json
import subprocess
import sys
from pathlib import Path
from unittest.mock import Mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from mosaico_cli.errors import OperationError, OutcomeUnknownError
from mosaico_cli.gateway import _wait_gateway_operation


def test_admission_rejection_keeps_the_resource_error_and_never_replays():
    body = {"operation_id": "intent", "submission_state": "rejected", "write_started": False,
            "http_status": 429, "error": {"code": "resource_limit", "message": "upload busy"}}
    context = Mock(log_path=Path("raw.log"))
    with pytest.raises(OperationError, match="upload busy") as caught:
        _wait_gateway_operation(context, Mock(), result=subprocess.CompletedProcess(
            [], 2, json.dumps(body), ""), started=0, timeout=30,
            action="System update", progress_prefix="system update")
    assert not isinstance(caught.value, OutcomeUnknownError)
    assert caught.value.details["write_started"] is False
    assert caught.value.details["result"] == body
    context.run.assert_not_called()

"""Regression coverage for bounded polling of asynchronous native UI state."""
from unittest.mock import Mock, patch

import pytest

from test_recovery_ui_host import Simulator


def simulator(tmp_path, snapshots):
    sim = Simulator.__new__(Simulator)
    sim.directory = tmp_path
    (tmp_path / "sim.log").write_text("backend running", encoding="utf-8")
    sim.backend = Mock()
    sim.backend.poll.return_value = None
    sim.state_file = Mock()
    sim.state_file.read_text.side_effect = snapshots
    sim.rpc = Mock()
    return sim


def test_snapshot_replacement_retries_without_losing_expected_state(tmp_path):
    sim = simulator(tmp_path, [PermissionError(), FileNotFoundError(), '{"page":8}'])
    with patch("test_recovery_ui_host.time.sleep"):
        assert sim.wait(page=8) == {"page": 8}
    assert sim.state_file.read_text.call_count == 3


def test_snapshot_permission_failure_remains_bounded(tmp_path):
    sim = simulator(tmp_path, PermissionError())
    with patch("test_recovery_ui_host.time.monotonic", side_effect=[0, 1, 16]), \
            patch("test_recovery_ui_host.time.sleep"), \
            pytest.raises(pytest.fail.Exception, match="expected"):
        sim.wait(page=8)


def test_update_can_complete_after_default_deadline(tmp_path):
    sim = simulator(tmp_path, ['{"page":7,"progress":900}', '{"page":8,"progress":1000}'])
    with patch("test_recovery_ui_host.time.monotonic", side_effect=[0, 16, 20]), \
            patch("test_recovery_ui_host.time.sleep"):
        assert sim.wait(timeout=45, page=8, progress=1000)["progress"] == 1000

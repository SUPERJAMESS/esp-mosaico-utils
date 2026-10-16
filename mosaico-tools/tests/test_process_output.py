"""Bounded build output remains complete on disk, including long lines."""
import os
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from mosaico_cli.process_output import CHUNK_BYTES, TAIL_BYTES, stream_process
from mosaico_cli.runtime import RunContext


def test_large_output_and_slow_progress_keep_complete_disk_copy(tmp_path):
    path = tmp_path / "stdout.log"
    count = 50000
    script = f"import os; os.write(1, b'line\\n' * {count}); os.write(1, b'x' * {TAIL_BYTES * 20}); print('end')"
    lengths = []
    def report(line):
        lengths.append(len(line))
        time.sleep(.001)
    result = stream_process([sys.executable, "-c", script], path=path, timeout=10, cwd=None, env=None, on_line=report)
    assert result.returncode == 0 and result.stdout_truncated and result.progress_dropped > 0
    assert len(result.stdout) <= TAIL_BYTES and result.stdout.endswith("end\n")
    assert max(lengths) <= CHUNK_BYTES
    assert path.read_bytes() == b"line\n" * count + b"x" * (TAIL_BYTES * 20) + b"end" + os.linesep.encode()
    assert result.stdout_path == str(path)


def test_timeout_reaps_child_and_keeps_diagnostic_tail(tmp_path):
    path = tmp_path / "timeout.log"
    with pytest.raises(subprocess.TimeoutExpired) as caught:
        stream_process([sys.executable, "-c", "import time; print('started', flush=True); time.sleep(30)"],
                       path=path, timeout=.3, cwd=None, env=None, on_line=lambda line: None)
    assert caught.value.output == "started\n"
    assert caught.value.stdout_path == str(path)
    assert path.read_text() == "started\n"
    assert not caught.value.stdout_truncated


def test_progress_failure_stops_child_and_checked_result_links_full_output(tmp_path):
    context = RunContext(SimpleNamespace(root=tmp_path, run_dir=tmp_path / "runs"), "test", json_output=True)
    def fail(line):
        raise RuntimeError("UI failure")
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="UI failure"):
        context.run([sys.executable, "-c", "import time; print('ready', flush=True); time.sleep(30)"], output_status=fail)
    assert time.monotonic() - started < 3
    with pytest.raises(subprocess.CalledProcessError) as caught:
        context.run([sys.executable, "-c", "print('diagnostic'); raise SystemExit(3)"],
                    check=True, output_status=lambda line: None)
    assert caught.value.output == "diagnostic\n"
    assert Path(caught.value.stdout_path).read_text() == "diagnostic\n"


def test_sensitive_stream_is_not_spooled_or_sent_to_progress(tmp_path):
    context = RunContext(SimpleNamespace(root=tmp_path, run_dir=tmp_path / "runs"), "test", json_output=True)
    lines = []
    result = context.run([sys.executable, "-c", "print('private-token')"], sensitive_output=True, output_status=lines.append)
    assert result.stdout == "private-token\n" and result.stdout_path is None
    assert lines == [] and not list(context.directory.glob("stdout-*.log"))
    assert "private-token" not in context.log_path.read_text()


def test_structured_nonstreaming_output_remains_complete(tmp_path):
    import json

    context = RunContext(SimpleNamespace(root=tmp_path, run_dir=tmp_path / "runs"), "test", json_output=True)
    result = context.run([sys.executable, "-c", f"import json; print(json.dumps({{'payload': 'x' * {TAIL_BYTES * 2}}}))"])
    assert json.loads(result.stdout) == {"payload": "x" * (TAIL_BYTES * 2)}

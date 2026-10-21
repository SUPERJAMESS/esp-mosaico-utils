"""A contender must never write metadata before acquiring the endpoint lock."""
import errno
import sys
from types import SimpleNamespace

import pytest

from iris_gateway import link
from iris_gateway.link import EndpointLock


def test_contender_during_owner_metadata_replacement(tmp_path):
    endpoint = "project-start:metadata-race"
    owner = EndpointLock(endpoint, root=tmp_path)
    contender = None
    try:
        owner.acquire()
        # Pause at the real acquire() window between truncate and metadata write.
        owner._file.seek(0)
        owner._file.truncate()
        owner._file.flush()
        contender = EndpointLock(endpoint, root=tmp_path)
        assert owner.path.stat().st_size == 0
        assert EndpointLock.held(endpoint, tmp_path)
        with pytest.raises(RuntimeError, match="owned by another"):
            contender.acquire()
        owner.close()
        contender.acquire()
        contender._file.seek(0)
        assert b"pid=" in contender._file.read()
    finally:
        owner.close()
        if contender is not None:
            contender.close()


def test_windows_blocking_lock_waits_past_ten_contended_attempts(tmp_path, monkeypatch):
    attempts = []
    sleeps = []

    def locking(fd, mode, size):
        if mode == 1:
            attempts.append(fd)
            if len(attempts) <= 11:
                raise OSError(errno.EACCES, "contended")

    lock = EndpointLock("project-start:queued-clients", root=tmp_path)
    monkeypatch.setattr(link, "sys", SimpleNamespace(platform="win32"))
    monkeypatch.setitem(sys.modules, "msvcrt",
                        SimpleNamespace(locking=locking, LK_NBLCK=1, LK_UNLCK=2))
    monkeypatch.setattr(link, "time", SimpleNamespace(sleep=sleeps.append))
    try:
        lock.acquire(blocking=True)
        assert len(attempts) == 12
        assert len(sleeps) == 11
    finally:
        lock.close()

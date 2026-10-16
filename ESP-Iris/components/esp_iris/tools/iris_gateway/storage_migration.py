"""Offline, restartable relocation of 0.2 diagnostics; source remains a backup."""
from __future__ import annotations

import contextlib
import json
import os
import pathlib
import shutil
import sqlite3
import time
from typing import Any

from .link import EndpointLock
from .schema import SCHEMA_VERSION, initialize_schema


def migrate_project_data(source: pathlib.Path, target: pathlib.Path) -> dict[str, Any]:
    source, target = source.resolve(), target.resolve()
    if source == target or source in target.parents or target in source.parents:
        raise ValueError("source and target data directories must be separate")
    stage = target.with_name("." + target.name + ".importing")
    marker = {"source": str(source), "target": str(target)}
    with contextlib.ExitStack() as stack:
        for root in sorted((source, target)):
            lock = EndpointLock("gateway-state:" + os.path.normcase(str(root)))
            stack.callback(lock.close)
            lock.acquire()
        if target.exists():
            raise FileExistsError("migration target already exists; no history will be overwritten")
        with contextlib.closing(sqlite3.connect((source / "gateway.sqlite3").as_uri() + "?mode=ro", uri=True)) as old:
            version = old.execute("PRAGMA user_version").fetchone()[0]
            if version not in {200, 201, SCHEMA_VERSION}:
                raise ValueError("only supported Iris 0.2 state can be relocated")
            if stage.exists():
                saved = json.loads((stage / "migration-source.json").read_text(encoding="utf-8"))
                if saved != marker:
                    raise ValueError("unfinished migration belongs to a different source")
                shutil.rmtree(stage)
            stage.mkdir(parents=True, mode=0o700)
            (stage / "migration-source.json").write_text(json.dumps(marker), encoding="utf-8")
            try:
                with contextlib.closing(sqlite3.connect(stage / "gateway.sqlite3")) as new:
                    old.backup(new)
                    initialize_schema(new)
                    new.execute("PRAGMA journal_mode=DELETE")
                    new.execute("PRAGMA secure_delete=ON")
                    with new:
                        new.execute("DELETE FROM agent_tokens")
                        new.execute("DELETE FROM settings WHERE key IN ('password_hash','host_id','tls')")
                    # Remove credential remnants from free pages in the portable copy.
                    new.execute("VACUUM")
                    if new.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                        raise RuntimeError("copied state failed SQLite integrity validation")
                for name in ("logs", "artifacts", "events"):
                    folder = source / name
                    if folder.exists():
                        if folder.is_symlink() or any(path.is_symlink() for path in folder.rglob("*")):
                            raise ValueError("diagnostic migration does not follow symlinks")
                        shutil.copytree(folder, stage / name)
                report = {**marker, "schema_version": SCHEMA_VERSION,
                          "created_ns": time.time_ns(), "source_retained": True,
                          "credentials_copied": False}
                (stage / "migration.json").write_text(json.dumps(report) + "\n", encoding="utf-8")
                for path in stage.rglob("*"):
                    if path.is_file():
                        # Windows _commit requires a writable descriptor.
                        with path.open("rb+") as handle:
                            os.fsync(handle.fileno())
                stage.replace(target)
                return report
            except BaseException:
                shutil.rmtree(stage)
                raise

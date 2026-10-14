"""Persistent state for the explicitly local Ideas integration server."""
from __future__ import annotations

from contextlib import contextmanager
import json
from pathlib import Path
import sqlite3
import time
import uuid

from .ideas_content import ContentError, digest, encoded, require, version_satisfies
from .ideas_release import validate_release


class Store:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        for name in ("archives", "blobs", "temporary"):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        with self.db() as db:
            db.executescript('''
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS uploads (
                  id TEXT PRIMARY KEY, owner TEXT NOT NULL, idem TEXT NOT NULL, request TEXT NOT NULL,
                  state TEXT NOT NULL, response TEXT, updated REAL NOT NULL, UNIQUE(owner, idem));
                CREATE TABLE IF NOT EXISTS applications (
                  number INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL,
                  owner TEXT NOT NULL, version INTEGER NOT NULL, draft TEXT);
                CREATE TABLE IF NOT EXISTS releases (
                  seq INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT UNIQUE NOT NULL,
                  application TEXT NOT NULL, code TEXT NOT NULL, root_hash TEXT NOT NULL,
                  firmware TEXT NOT NULL, profile TEXT NOT NULL, published REAL, visible INTEGER DEFAULT 0);
                CREATE TABLE IF NOT EXISTS release_files (
                  release TEXT NOT NULL, hash TEXT NOT NULL, PRIMARY KEY(release, hash));
                CREATE TABLE IF NOT EXISTS settings (name TEXT PRIMARY KEY, value TEXT NOT NULL);
            ''')

    def account_id(self, initial_id):
        """One fixture account, stable across test-token rotation and restart."""
        with self.db() as db:
            db.execute("INSERT OR IGNORE INTO settings VALUES ('account',?)", (initial_id,))
            return db.execute("SELECT value FROM settings WHERE name='account'").fetchone()[0]

    @contextmanager
    def db(self):
        connection = sqlite3.connect(self.root / "state.sqlite3", timeout=30)
        connection.row_factory = sqlite3.Row
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def put(self, group, data):
        name = digest(data)
        target = self.root / group / name
        if target.exists():
            require(digest(target.read_bytes()) == name, "Immutable object was modified", code="IMMUTABILITY_CONFLICT")
        else:
            temporary = self.root / "temporary" / uuid.uuid4().hex
            try:
                with temporary.open("xb") as file:
                    file.write(data)
                    file.flush()
                    import os
                    os.fsync(file.fileno())
                # Concurrent writers of the same hash have exactly the same bytes.
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
        return name

    def begin(self, owner, key, request):
        canonical = encoded(request).decode()
        with self.db() as db:
            db.execute("BEGIN IMMEDIATE")
            existing = db.execute("SELECT * FROM uploads WHERE owner=? AND idem=?", (owner, key)).fetchone()
            if existing:
                require(existing["request"] == canonical, "Idempotency key has different input", code="IDEMPOTENCY_CONFLICT")
                require(existing["state"] != "receiving", "Upload still receiving", code="UPLOAD_IN_PROGRESS")
                require(existing["state"] != "expired", "Upload record expired", code="UPLOAD_EXPIRED")
                if existing["state"] != "retryable":
                    return existing["id"], True
                db.execute("UPDATE uploads SET state='receiving',updated=? WHERE id=?", (time.time(), existing["id"]))
                return existing["id"], False
            if request["mode"] == "update":
                app = db.execute("SELECT * FROM applications WHERE id=?", (request["application_id"],)).fetchone()
                require(app is not None, "Unknown application", code="APPLICATION_NOT_FOUND")
                require(app["owner"] == owner, "Application belongs to another owner", code="FORBIDDEN")
                require(app["version"] == request["expected_project_version"], "Draft version changed", code="PROJECT_VERSION_CONFLICT")
            identifier = "upload-" + uuid.uuid4().hex
            db.execute("INSERT INTO uploads VALUES (?,?,?,?,?,?,?)",
                       (identifier, owner, key, canonical, "receiving", None, time.time()))
            return identifier, False

    def received(self, identifier, raw):
        self.put("archives", raw)
        with self.db() as db:
            db.execute("UPDATE uploads SET state='validating',updated=? WHERE id=? AND state='receiving'", (time.time(), identifier))

    def interrupted(self, identifier):
        with self.db() as db:
            db.execute("UPDATE uploads SET state='retryable',updated=? WHERE id=? AND state='receiving'", (time.time(), identifier))

    def status(self, identifier, owner):
        with self.db() as db:
            row = db.execute("SELECT * FROM uploads WHERE id=? AND owner=?", (identifier, owner)).fetchone()
            require(row is not None, "Upload not found", code="UPLOAD_NOT_FOUND")
            if row["state"] == "failed" and row["updated"] + 7*86400 <= time.time():
                db.execute("UPDATE uploads SET state='expired',response=NULL WHERE id=?", (identifier,))
                db.commit()
                raise ContentError("UPLOAD_EXPIRED", "Upload record expired")
            require(row["state"] != "expired", "Upload record expired", code="UPLOAD_EXPIRED")
            result = {"upload_id": identifier, "status": row["state"],
                      "archive_sha256": json.loads(row["request"])["archive"]["sha256"]}
            if row["response"]:
                result["result" if row["state"] == "complete" else "error"] = json.loads(row["response"])
            return result

    def pending(self):
        with self.db() as db:
            return [row[0] for row in db.execute("SELECT id FROM uploads WHERE state='validating'")]

    def validate(self, identifier):
        with self.db() as db:
            upload = db.execute("SELECT * FROM uploads WHERE id=? AND state='validating'", (identifier,)).fetchone()
        if upload is None:
            return
        request = json.loads(upload["request"])
        try:
            release = validate_release(self.root / "archives" / request["archive"]["sha256"])
            require(release.archive_sha256 == request["archive"]["sha256"], "Archive storage changed", code="ARCHIVE_HASH_MISMATCH")
            with self.db() as db:
                db.execute("BEGIN IMMEDIATE")
                if db.execute("SELECT state FROM uploads WHERE id=?", (identifier,)).fetchone()[0] != "validating":
                    return
                if request["mode"] == "create":
                    application_id = "app-" + uuid.uuid4().hex
                    cursor = db.execute("INSERT INTO applications(id,owner,version) VALUES (?,?,0)", (application_id, upload["owner"]))
                    require(cursor.lastrowid <= 999999, "No application codes available", code="SERVICE_UNAVAILABLE")
                    number, version = cursor.lastrowid, 1
                else:
                    app = db.execute("SELECT * FROM applications WHERE id=?", (request["application_id"],)).fetchone()
                    require(app is not None and app["owner"] == upload["owner"], "Ownership changed", code="FORBIDDEN")
                    require(app["version"] == request["expected_project_version"], "Draft version changed", code="PROJECT_VERSION_CONFLICT")
                    application_id, number, version = app["id"], app["number"], app["version"]+1
                code, release_id = f"{number:06}", "release-" + uuid.uuid4().hex
                _, files = release.device_release(application_id, code, release_id)
                root_hash = digest(files["manifest.json"])
                hashes = {self.put("blobs", data) for data in files.values()}
                db.execute("INSERT INTO releases(id,application,code,root_hash,firmware,profile) VALUES (?,?,?,?,?,?)",
                           (release_id, application_id, code, root_hash, encoded(release.firmware).decode(), release.manifest["device"]["profile"]))
                db.executemany("INSERT INTO release_files VALUES (?,?)", [(release_id, h) for h in hashes])
                db.execute("UPDATE applications SET version=?,draft=? WHERE id=?", (version, release_id, application_id))
                result = {"application_id": application_id, "code": code, "revision_id": release_id,
                          "release_id": release_id, "project_version": version, "version": release.firmware["release"],
                          "archive_sha256": release.archive_sha256, "manifest_sha256": digest(release.files["manifest.json"]),
                          "bundle_sha256": release.manifest["firmware"]["sha256"], "status": "draft",
                          "draft_url": "/test/drafts/" + release_id}
                db.execute("UPDATE uploads SET state='complete',response=?,updated=? WHERE id=?",
                           (encoded(result).decode(), time.time(), identifier))
        except ContentError as error:
            with self.db() as db:
                detail = {"code": error.code, "message": str(error), "request_id": identifier, "details": {"path": error.path}}
                db.execute("UPDATE uploads SET state='failed',response=?,updated=? WHERE id=? AND state='validating'",
                           (encoded(detail).decode(), time.time(), identifier))

    def publish(self, release_id):
        with self.db() as db:
            require(db.execute("SELECT id FROM releases WHERE id=?", (release_id,)).fetchone(), "Release does not exist")
            db.execute("UPDATE releases SET published=COALESCE(published,?),visible=1 WHERE id=?", (time.time(), release_id))

    def manifest(self, code, capabilities=None, release_id=None):
        with self.db() as db:
            if release_id:
                rows = db.execute("SELECT * FROM releases WHERE code=? AND id=? AND published IS NOT NULL", (code, release_id)).fetchall()
            else:
                rows = db.execute("SELECT * FROM releases WHERE code=? AND visible=1 ORDER BY published DESC,seq DESC", (code,)).fetchall()
            require(rows, "Published release not found", code="APPLICATION_NOT_FOUND")
            for row in rows:
                firmware = json.loads(row["firmware"])
                if capabilities and not (row["profile"] == capabilities["profile"]
                        and firmware["target"]["chip_id"] == capabilities["chip_id"]
                        and firmware["target"]["flash_size"] == capabilities["flash_size"]
                        and version_satisfies(capabilities["recovery_version"], firmware.get("minimum_recovery_version"))):
                    continue
                return (self.root / "blobs" / row["root_hash"]).read_bytes(), row["root_hash"]
        raise ContentError("NO_COMPATIBLE_RELEASE", "No published release matches the device")

    def blob(self, sha256):
        with self.db() as db:
            visible = db.execute("SELECT 1 FROM release_files f JOIN releases r ON r.id=f.release WHERE f.hash=? AND r.published IS NOT NULL LIMIT 1", (sha256,)).fetchone()
        require(visible, "Published file not found", code="FILE_NOT_FOUND")
        return self.root / "blobs" / sha256

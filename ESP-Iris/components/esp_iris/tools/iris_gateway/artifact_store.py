from __future__ import annotations

import hashlib
import json
import pathlib
import time
from typing import Any

from .state_store import StateStore
from .storage_json import decode_json as _loads
from .storage_json import encode_json as _json


class ArtifactStore:
    """Explicitly retained firmware and crash evidence, outside log retention."""

    def __init__(self, state: StateStore, root: pathlib.Path) -> None:
        self.db = state.db
        self.artifacts_dir = root / "artifacts"
        self.artifacts_dir.mkdir(exist_ok=True)

    def save_firmware_artifact(
        self,
        *,
        binary: bytes,
        elf: bytes,
        map_data: bytes,
        metadata: dict[str, Any],
    ) -> dict[str, Any]:
        """Persist one complete, content-addressed firmware evidence bundle."""

        binary_sha = hashlib.sha256(binary).hexdigest()
        elf_sha = hashlib.sha256(elf).hexdigest()
        map_sha = hashlib.sha256(map_data).hexdigest()
        artifact_id = elf_sha
        existing = self.firmware_artifact(artifact_id)
        if existing is not None:
            if existing["binary_sha256"] != binary_sha:
                raise ValueError("ELF SHA already exists with a different firmware binary")
            if existing.get("map_sha256") != map_sha:
                raise ValueError("ELF SHA already exists with a different linker map")
            return existing

        folder = self.artifacts_dir / "firmware" / artifact_id
        folder.mkdir(parents=True, exist_ok=True)
        files = {
            "firmware.bin": binary,
            "firmware.elf": elf,
            "firmware.map": map_data,
        }
        for name, data in files.items():
            target = folder / name
            try:
                with target.open("xb") as handle:
                    handle.write(data)
            except FileExistsError:
                if hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(data).digest():
                    raise ValueError(f"artifact file collision: {target}")
        manifest = {
            "schema": "esp-iris-firmware-artifact/v1",
            "artifact_id": artifact_id,
            "binary_sha256": binary_sha,
            "elf_sha256": elf_sha,
            "map_sha256": map_sha,
            "project_name": str(metadata["project_name"]),
            "version": str(metadata["version"]),
            "chip_id": int(metadata["chip_id"]),
            "sizes": {name: len(data) for name, data in files.items()},
            "sha256": {
                name: hashlib.sha256(data).hexdigest()
                for name, data in files.items()
            },
            "files": {name: name for name in files},
            "created_ns": time.time_ns(),
        }
        (folder / "manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        self.db.execute(
            """INSERT INTO firmware_artifacts(
                   artifact_id, binary_sha256, elf_sha256, project_name,
                   version, manifest_json, created_ns)
               VALUES(?, ?, ?, ?, ?, ?, ?)""",
            (
                artifact_id,
                binary_sha,
                elf_sha,
                manifest["project_name"],
                manifest["version"],
                _json(manifest),
                manifest["created_ns"],
            ),
        )
        self.db.commit()
        return {**manifest, "path": str(folder)}

    def firmware_artifact(self, artifact_id: str) -> dict[str, Any] | None:
        row = self.db.execute(
            "SELECT * FROM firmware_artifacts WHERE artifact_id=?", (artifact_id,)
        ).fetchone()
        if row is None:
            return None
        manifest = _loads(row["manifest_json"], {})
        folder = self.artifacts_dir / "firmware" / str(row["artifact_id"])
        return {**manifest, "path": str(folder)}

    def firmware_artifacts(self, limit: int = 200) -> list[dict[str, Any]]:
        rows = self.db.execute(
            "SELECT artifact_id FROM firmware_artifacts ORDER BY created_ns DESC LIMIT ?",
            (max(1, min(limit, 1000)),),
        ).fetchall()
        return [
            artifact
            for row in rows
            if (artifact := self.firmware_artifact(str(row["artifact_id"]))) is not None
        ]

    def save_artifact(
        self, device_id: str, kind: str, data: bytes, suffix: str
    ) -> pathlib.Path:
        safe_device = "".join(c for c in device_id if c.isalnum() or c in "-_")
        folder = self.artifacts_dir / safe_device
        folder.mkdir(exist_ok=True)
        created_ns = time.time_ns()
        stamp = time.strftime(
            "%Y%m%d-%H%M%S", time.localtime(created_ns // 1_000_000_000)
        )
        digest = hashlib.sha256(data).hexdigest()[:12]
        nanoseconds = created_ns % 1_000_000_000
        stem = f"{stamp}-{nanoseconds:09d}-{kind}-{digest}"
        extension = suffix.lstrip(".")
        for collision in range(1_000):
            discriminator = f"-{collision}" if collision else ""
            target = folder / f"{stem}{discriminator}.{extension}"
            try:
                with target.open("xb") as handle:
                    handle.write(data)
                return target
            except FileExistsError:
                continue
        raise FileExistsError("could not allocate a unique artifact filename")

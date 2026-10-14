"""Single immutable ZIP upload; persistent receipt and idempotency binding."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
import sys
import time
import uuid

from .errors import SelectionError
from .ideas_content import require
from .ideas_release import validate_release
from .ideas_transport import ReleaseClient
from .platform_client import PlatformError, credential, private_root, read_state, server_url, write_state


def upload_archive(client, package, request, ledger_path, *, wait_seconds=600, poll_seconds=2):
    ledger = read_state(ledger_path)
    if not ledger:
        ledger = {"key": str(uuid.uuid4()), "request": request, "package": str(package), "server": client.server}
        write_state(ledger_path, ledger)
    require(ledger["request"] == request and ledger["server"] == client.server,
            "Upload state does not match the frozen archive and target")
    if ledger.get("status") == "complete":
        return ledger["result"]
    if ledger.get("status") == "failed":
        if ledger.get("finished_at", 0) + 7*86400 <= time.time():
            raise PlatformError("UPLOAD_EXPIRED")
        raise PlatformError(ledger["error"]["code"])
    if ledger.get("upload_id"):
        response = client.request("GET", "/ideas/uploads/" + ledger["upload_id"])
    else:
        response = client.request("POST", "/ideas/uploads", request, package=package, key=ledger["key"])
    identifier = response.get("upload_id")
    require(isinstance(identifier, str) and 0 < len(identifier) <= 128
            and all(c.isalnum() or c in "-_" for c in identifier), "Invalid upload identity")
    require(not ledger.get("upload_id") or ledger["upload_id"] == identifier, "Upload identity changed")
    ledger["upload_id"] = identifier
    write_state(ledger_path, ledger)
    deadline, interval = time.monotonic() + wait_seconds, poll_seconds
    while True:
        status = response.get("status")
        require(status in ("validating", "complete", "failed") and response.get("upload_id") == identifier,
                "Invalid upload state response")
        if response.get("archive_sha256") is not None:
            require(response["archive_sha256"] == request["archive"]["sha256"], "Upload archive identity changed")
        if status in ("complete", "failed"):
            field = "result" if status == "complete" else "error"
            require(isinstance(response.get(field), dict), "Missing upload result")
            require(response.get("archive_sha256") == request["archive"]["sha256"], "Missing upload archive identity")
            if status == "complete":
                result = response[field]
                require(result.get("status") == "draft", "Upload unexpectedly published content")
                for name in ("application_id", "revision_id", "release_id"):
                    require(isinstance(result.get(name), str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", result[name]),
                            "Invalid result identity", name)
                require(isinstance(result.get("code"), str) and re.fullmatch(r"[0-9]{6}", result["code"]), "Invalid application code")
                require(type(result.get("project_version")) is int and result["project_version"] > 0, "Invalid draft version")
                for name in ("manifest_sha256", "bundle_sha256"):
                    require(isinstance(result.get(name), str) and re.fullmatch(r"[0-9a-f]{64}", result[name]), "Invalid result digest", name)
                if request["mode"] == "update":
                    require(result["application_id"] == request["application_id"] and
                            result["project_version"] == request["expected_project_version"] + 1, "Upload target changed")
            else:
                code = response[field].get("code")
                require(isinstance(code, str) and re.fullmatch(r"[A-Z][A-Z0-9_]{0,79}", code), "Invalid failure code")
            ledger.update(status=status, finished_at=time.time(), **{field: response[field]})
            write_state(ledger_path, ledger)
            if status == "failed":
                raise PlatformError(response[field].get("code", "VALIDATION_FAILED"))
            return response[field]
        if time.monotonic() + interval >= deadline:
            return {"status": "validating", "upload_id": identifier, "state_file": str(ledger_path)}
        time.sleep(interval)
        interval = min(10, interval*2)
        response = client.request("GET", "/ideas/uploads/" + identifier, deadline=deadline)


def project_upload(arguments, context):
    if not arguments.create and not arguments.update:
        raise SelectionError("Choose --create or --update APPLICATION_ID explicitly.")
    if arguments.update and arguments.expected_project_version is None:
        raise SelectionError("--update requires --expected-project-version for concurrency protection.")
    if arguments.create and arguments.expected_project_version is not None:
        raise SelectionError("--expected-project-version is only valid with --update.")
    if not arguments.yes:
        if arguments.json or not sys.stdin.isatty() or input("Upload this ZIP as an Ideas draft? [y/N] ").strip().lower() != "y":
            raise SelectionError("Use --yes to confirm the explicit draft upload.")
    package = Path(arguments.package).expanduser().resolve()
    release = validate_release(package)
    server = server_url(arguments.server)
    token = credential(server)
    client = ReleaseClient(server, token, ca_file=arguments.ca_file)
    request = {"mode": "create" if arguments.create else "update",
               "archive": {"size": package.stat().st_size, "sha256": release.archive_sha256}}
    if arguments.update:
        require(arguments.expected_project_version >= 0, "Expected project version must be nonnegative")
        request.update(application_id=arguments.update, expected_project_version=arguments.expected_project_version)
    identity = client.request("GET", "/auth/cli/token").get("user", {})
    owner = identity.get("id") if isinstance(identity, dict) else None
    require(isinstance(owner, str) and 0 < len(owner) <= 128, "Account response has no stable user ID")
    fingerprint = hashlib.sha256((owner + "\0" + json.dumps(request, sort_keys=True)).encode()).hexdigest()
    ledger_path = private_root(server) / ("zip-upload-" + fingerprint + ".json")
    result = upload_archive(client, package, request, ledger_path, wait_seconds=arguments.wait_timeout)
    if result.get("status") == "draft":
        require(result.get("manifest_sha256") == hashlib.sha256(release.files["manifest.json"]).hexdigest()
                and result.get("bundle_sha256") == release.manifest["firmware"]["sha256"]
                and result.get("version") == release.firmware["release"], "Upload receipt does not match validated content")
    return {"command": "project upload", **result}

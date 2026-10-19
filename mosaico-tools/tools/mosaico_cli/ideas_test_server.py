"""Local integration server, with explicit publication and persistent transactions.

This is a protocol fixture server, not the production site's account/review UI.
Run with TLS for a device; unencrypted HTTP is restricted to loopback.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from email.message import Message
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import ssl
import threading
import time
from urllib.parse import parse_qs, urlsplit
import uuid

from .ideas_content import ARCHIVE_BYTES, ContentError, digest, encoded, require as content_require, strict_json, version_tuple
from .ideas_test_store import Store
from .ideas_preview import preview_response

def require(condition, message, path=None, *, code="INVALID_REQUEST"):
    content_require(condition, message, path, code=code)


ERROR_STATUS = {"AUTH_REQUIRED":401, "FORBIDDEN":403, "APPLICATION_NOT_FOUND":404,
    "UPLOAD_NOT_FOUND":404, "FILE_NOT_FOUND":404, "NO_COMPATIBLE_RELEASE":409,
    "IDEMPOTENCY_CONFLICT":409, "PROJECT_VERSION_CONFLICT":409, "UPLOAD_IN_PROGRESS":409,
    "UPLOAD_EXPIRED":410, "ARCHIVE_TOO_LARGE":413, "PACKAGE_TOO_LARGE":413,
    "UNSUPPORTED_MEDIA_TYPE":415, "NOT_ACCEPTABLE":406, "ARCHIVE_HASH_MISMATCH":422,
    "FILE_SIZE_MISMATCH":422, "FILE_HASH_MISMATCH":422, "INVALID_PRESENTATION":422,
    "INVALID_FIRMWARE":422, "MISSING_FILES":422, "SERVICE_UNAVAILABLE":503}


class IdeasServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, store, token):
        self.store, self.token = store, token
        self.owner = store.account_id(hashlib.sha256(token.encode()).hexdigest())
        self.pool = ThreadPoolExecutor(max_workers=2, thread_name_prefix="ideas-validation")
        self.active, self.lock = set(), threading.Lock()
        self.receivers = threading.BoundedSemaphore(4)
        super().__init__(address, Handler)
        with store.db() as db:
            db.execute("UPDATE uploads SET state='retryable' WHERE state='receiving'")
        for identifier in store.pending():
            self.submit(identifier)

    def submit(self, identifier):
        with self.lock:
            if identifier in self.active:
                return
            self.active.add(identifier)
        def work():
            try:
                self.store.validate(identifier)
            except Exception as error:
                # Keep the durable validating state; a query/restart can resume it.
                print(json.dumps({"event":"validation_interrupted", "upload_id":identifier,
                                  "error_type":type(error).__name__}), flush=True)
            finally:
                with self.lock:
                    self.active.discard(identifier)
        self.pool.submit(work)

    def server_close(self):
        super().server_close()
        self.pool.shutdown(wait=True)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MosaicoIdeasTest/1"

    def log_message(self, fmt, *args):
        # Do not log Authorization or request bodies.
        print(json.dumps({"event":"http", "method":self.command,
                          "path":urlsplit(self.path).path, "message":fmt % args}), flush=True)

    def send_bytes(self, status, data, *, media="application/json", cache="no-store", etag=None, vary=None):
        self.send_response(status)
        self.send_header("Content-Type", media)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", cache)
        if etag:
            self.send_header("ETag", '"' + etag + '"')
        if vary:
            self.send_header("Vary", vary)
        if status in (429, 503) or status == 409 and b"UPLOAD_IN_PROGRESS" in data:
            self.send_header("Retry-After", "2")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    def failure(self, error):
        code = error.code
        self.send_bytes(ERROR_STATUS.get(code, 400), encoded({"code":code,
            "message":str(error), "request_id":uuid.uuid4().hex, "details":{"path":error.path}}))

    def authorized(self):
        require(hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + self.server.token),
                "A valid test account token is required", code="AUTH_REQUIRED")

    def do_GET(self):
        try:
            parsed = urlsplit(self.path)
            if parsed.path == "/api/v1/auth/cli/token":
                self.authorized()
                require(not parsed.query, "Unexpected query parameters", code="INVALID_REQUEST")
                self.send_bytes(200, encoded({"user":{"id":self.server.owner}}))
                return
            match = re.fullmatch(r"/api/v1/ideas/uploads/([A-Za-z0-9_-]{1,128})", parsed.path)
            if match:
                self.authorized()
                require(not parsed.query, "Unexpected query parameters")
                response = self.server.store.status(match[1], self.server.owner)
                if response["status"] == "validating":
                    self.server.submit(match[1])
                self.send_bytes(200, encoded(response))
                return
            match = re.fullmatch(r"/api/v1/ideas/([0-9]{6})(?:/releases/([A-Za-z0-9][A-Za-z0-9._-]{0,63}))?/manifest.json", parsed.path)
            if match:
                capabilities = None
                locale = None
                require(len(self.headers.get_all("Accept", [])) <= 1, "Duplicate Accept header")
                accept = self.headers.get("Accept", "application/json")
                require(accept in ("application/json", "multipart/related", "*/*"),
                        "Unsupported manifest representation", code="NOT_ACCEPTABLE")
                if match[2]:
                    require(not parsed.query, "Fixed release requests cannot select another version")
                    require(accept != "multipart/related", "Fixed release returns the raw JSON manifest")
                else:
                    fields = parse_qs(parsed.query, keep_blank_values=True, max_num_fields=8)
                    expected = {"profile", "chip_id", "flash_size", "recovery_version"}
                    if accept == "multipart/related":
                        expected.add("locale")
                    require(set(fields) == expected
                            and all(len(v) == 1 and v[0] for v in fields.values()), "Missing, duplicate or unknown capability parameter")
                    if accept == "multipart/related":
                        locale = fields.pop("locale")[0]
                        require(locale in ("zh-CN", "en"), "Invalid preview locale")
                    capabilities = {k:v[0] for k,v in fields.items()}
                    for name in ("chip_id", "flash_size"):
                        require(re.fullmatch(r"[0-9]{1,10}", capabilities[name]) is not None, "Invalid device capability")
                        capabilities[name] = int(capabilities[name])
                    require(0 < capabilities["chip_id"] <= 65535 and 0 < capabilities["flash_size"] <= 32*1024*1024
                            and capabilities["flash_size"] % 4096 == 0, "Invalid device hardware capability")
                    version_tuple(capabilities["recovery_version"])
                raw, sha = self.server.store.manifest(match[1], capabilities, match[2])
                media = "application/json"
                if locale:
                    raw, media = preview_response(raw, locale, lambda sha: self.server.store.blob(sha).read_bytes())
                    sha = digest(raw)
                self.send_bytes(200, raw, media=media, etag=sha, vary="Accept",
                                cache="public, max-age=31536000, immutable" if match[2] else "no-store")
                return
            match = re.fullmatch(r"/api/v1/ideas/files/([0-9a-f]{64})", parsed.path)
            if match:
                fields = parse_qs(parsed.query, keep_blank_values=True, max_num_fields=1)
                require(not fields or (set(fields) == {"device_id"} and
                        len(fields["device_id"]) == 1 and 1 <= len(fields["device_id"][0]) <= 128),
                        "Invalid device_id query")
                file = self.server.store.blob(match[1])
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(file.stat().st_size))
                self.send_header("Cache-Control", "public, max-age=31536000, immutable")
                self.send_header("Connection", "close")
                self.end_headers()
                with file.open("rb") as stream:
                    while block := stream.read(65536):
                        self.wfile.write(block)
                self.close_connection = True
                return
            raise ContentError("FILE_NOT_FOUND", "Unknown test endpoint")
        except ContentError as error:
            self.failure(error)
        except (ValueError, UnicodeError):
            self.failure(ContentError("INVALID_REQUEST", "Malformed query"))

    def do_POST(self):
        identifier, duplicate, acquired = None, False, False
        try:
            self.authorized()
            require(self.path == "/api/v1/ideas/uploads", "Unknown upload endpoint")
            acquired = self.server.receivers.acquire(blocking=False)
            require(acquired, "Test server upload capacity reached", code="SERVICE_UNAVAILABLE")
            self.connection.settimeout(60)
            lengths = self.headers.get_all("Content-Length", [])
            require(len(lengths) == 1 and lengths[0].isdigit() and not self.headers.get("Transfer-Encoding"),
                    "A single Content-Length is required")
            length = int(lengths[0])
            require(0 < length <= ARCHIVE_BYTES + 16384, "Multipart request too large", code="ARCHIVE_TOO_LARGE")
            media = Message()
            media["content-type"] = self.headers.get("Content-Type", "")
            boundary = media.get_param("boundary")
            require(media.get_content_type() == "multipart/form-data" and isinstance(boundary, str)
                    and re.fullmatch(r"[A-Za-z0-9_-]{1,70}", boundary), "Invalid multipart content type", code="UNSUPPORTED_MEDIA_TYPE")
            boundary = boundary.encode()
            used, deadline = 0, time.monotonic()+1800
            def read(count, line=False):
                nonlocal used
                require(used+count <= length if not line else used < length, "Truncated multipart request")
                if time.monotonic() >= deadline:
                    raise TimeoutError()
                result = self.rfile.readline(min(count, length-used)) if line else self.rfile.read(count)
                used += len(result)
                require(bool(result) and (line or len(result) == count), "Truncated multipart body")
                return result
            def headers(name, expected):
                rows, size = {}, 0
                while True:
                    row = read(4097, True)
                    size += len(row)
                    require(size <= 4096 and row.endswith(b"\r\n"), "Multipart headers exceed limit")
                    if row == b"\r\n": break
                    key, separator, value = row.decode("ascii").partition(":")
                    key = key.lower()
                    require(separator and key not in rows, "Invalid multipart header")
                    rows[key] = value.strip()
                require(set(rows) == {"content-disposition", "content-type"} and rows["content-type"] == expected,
                        "Invalid multipart part type", code="UNSUPPORTED_MEDIA_TYPE")
                disposition = Message()
                disposition["content-disposition"] = rows["content-disposition"]
                require(disposition.get_content_disposition() == "form-data"
                        and disposition.get_param("name", header="content-disposition") == name,
                        "Unexpected multipart part")
            require(read(80, True) == b"--"+boundary+b"\r\n", "Multipart boundary mismatch")
            headers("request", "application/json")
            request_bytes = bytearray()
            while True:
                row = read(4097, True)
                if row == b"--"+boundary+b"\r\n": break
                request_bytes.extend(row)
                require(len(request_bytes) <= 4098, "Request metadata exceeds 4 KiB")
            require(request_bytes.endswith(b"\r\n"), "Missing request delimiter")
            request = strict_json(bytes(request_bytes[:-2]), 4096, path="request")
            required = {"mode", "archive"}
            if request.get("mode") == "update": required |= {"application_id", "expected_project_version"}
            require(request.get("mode") in ("create", "update") and set(request) == required,
                    "Invalid upload request fields")
            if request["mode"] == "update":
                require(isinstance(request["application_id"], str)
                        and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", request["application_id"])
                        and type(request["expected_project_version"]) is int and request["expected_project_version"] >= 0,
                        "Invalid update target or expected version")
            archive = request["archive"]
            require(isinstance(archive, dict) and set(archive) == {"size", "sha256"}
                    and type(archive["size"]) is int and 0 < archive["size"] <= ARCHIVE_BYTES
                    and isinstance(archive["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", archive["sha256"]),
                    "Invalid archive identity")
            keys = self.headers.get_all("Idempotency-Key", [])
            require(len(keys) == 1 and re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", keys[0]), "Invalid Idempotency-Key")
            headers("package", "application/zip")
            identifier, duplicate = self.server.store.begin(self.server.owner, keys[0], request)
            temporary = self.server.store.root / "temporary" / (uuid.uuid4().hex + ".zip")
            try:
                sha, received = hashlib.sha256(), 0
                with temporary.open("xb") as file:
                    while received < archive["size"]:
                        block = read(min(65536, archive["size"]-received))
                        received += len(block)
                        sha.update(block)
                        file.write(block)
                ending = b"\r\n--" + boundary + b"--\r\n"
                require(length-used == len(ending) and read(len(ending)) == ending, "Multipart must contain exactly two parts")
                require(sha.hexdigest() == archive["sha256"], "ZIP SHA-256 mismatch", code="ARCHIVE_HASH_MISMATCH")
                if not duplicate:
                    self.server.store.received(identifier, temporary.read_bytes())
            finally:
                temporary.unlink(missing_ok=True)
            response = self.server.store.status(identifier, self.server.owner)
            # First acceptance always reports validating, even if worker is fast.
            self.send_bytes(202 if response["status"] == "validating" else 200, encoded(response))
            if response["status"] == "validating":
                self.server.submit(identifier)
        except ContentError as error:
            if identifier and not duplicate:
                self.server.store.interrupted(identifier)
            self.failure(error)
        except (OSError, TimeoutError, ValueError, UnicodeError):
            if identifier and not duplicate:
                self.server.store.interrupted(identifier)
            self.close_connection = True
        finally:
            if acquired:
                self.server.receivers.release()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path, required=True)
    actions = parser.add_subparsers(dest="action", required=True)
    serve = actions.add_parser("serve")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8443)
    serve.add_argument("--token-file", type=Path, required=True)
    serve.add_argument("--cert", type=Path)
    serve.add_argument("--key", type=Path)
    publish = actions.add_parser("publish")
    publish.add_argument("release_id")
    args = parser.parse_args(argv)
    store = Store(args.state)
    if args.action == "publish":
        store.publish(args.release_id)
        print(json.dumps({"status":"published", "release_id":args.release_id}))
        return
    token = args.token_file.read_text().strip()
    require(token and not any(c.isspace() for c in token), "Invalid test token file")
    require(bool(args.cert) == bool(args.key), "TLS requires both certificate and key")
    require(args.cert or args.host in ("127.0.0.1", "localhost"), "Device-facing test servers require TLS")
    server = IdeasServer((args.host, args.port), store, token)
    if args.cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    print(json.dumps({"status":"listening", "host":args.host, "port":server.server_port, "tls":bool(args.cert)}), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()

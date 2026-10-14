"""Streaming ZIP transport with bounded attempts and upload identities."""
from __future__ import annotations

from email.utils import parsedate_to_datetime
import hashlib
import http.client
import json
import random
import ssl
import time
from urllib.parse import urlsplit
import uuid

from .ideas_content import encoded, require
from .platform_client import PlatformError, server_url


def retry_after(value):
    if not value:
        return 0.0
    try:
        return max(0.0, float(int(value))) if value.isdigit() else max(0.0, parsedate_to_datetime(value).timestamp() - time.time())
    except (ValueError, TypeError, OverflowError):
        return 0.0


class ReleaseClient:
    def __init__(self, server, token, *, ca_file=None):
        self.server, self.token = server_url(server), token
        self.context = ssl.create_default_context(cafile=ca_file)

    def _send(self, method, path, metadata=None, package=None, key=None, budget_deadline=None):
        origin = urlsplit(self.server)
        connection = (http.client.HTTPSConnection(origin.hostname, origin.port, timeout=10, context=self.context)
                      if origin.scheme == "https" else http.client.HTTPConnection(origin.hostname, origin.port, timeout=10))
        deadline = time.monotonic() + (1800 if package else 30)
        if budget_deadline is not None:
            deadline = min(deadline, budget_deadline)
        headers = {"Accept": "application/json", "Accept-Encoding": "identity"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if key:
            headers["Idempotency-Key"] = key
        prefix = suffix = b""
        if package:
            boundary = "mosaico-" + uuid.uuid4().hex
            prefix = ("--" + boundary + '\r\nContent-Disposition: form-data; name="request"\r\nContent-Type: application/json\r\n\r\n').encode() + encoded(metadata)
            prefix += ("\r\n--" + boundary + '\r\nContent-Disposition: form-data; name="package"; filename="release.zip"\r\nContent-Type: application/zip\r\n\r\n').encode()
            suffix = ("\r\n--" + boundary + "--\r\n").encode()
            headers["Content-Type"] = "multipart/form-data; boundary=" + boundary
            headers["Content-Length"] = str(len(prefix) + metadata["archive"]["size"] + len(suffix))
        elif metadata is not None:
            prefix = encoded(metadata)
            headers["Content-Type"] = "application/json"
            headers["Content-Length"] = str(len(prefix))

        def budget():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError()
            if connection.sock:
                connection.sock.settimeout(min(60, remaining))

        try:
            connection.connect()
            budget()
            connection.putrequest(method, "/api/v1" + path, skip_accept_encoding=True)
            for name, value in headers.items():
                connection.putheader(name, value)
            connection.endheaders()
            if prefix:
                connection.send(prefix)
            if package:
                check, total = hashlib.sha256(), 0
                with package.open("rb") as source:
                    while block := source.read(65536):
                        budget()
                        total += len(block)
                        require(total <= metadata["archive"]["size"], "ZIP changed after validation")
                        check.update(block)
                        connection.send(block)
                require(total == metadata["archive"]["size"] and check.hexdigest() == metadata["archive"]["sha256"],
                        "ZIP changed after validation")
                budget()
                connection.send(suffix)
            budget()
            response = connection.getresponse()
            data = bytearray()
            while True:
                budget()
                block = response.read1(16384)
                if not block:
                    break
                data.extend(block)
                if len(data) > 2*1024*1024:
                    raise PlatformError("INVALID_RESPONSE")
            try:
                value = json.loads(data)
            except (ValueError, UnicodeError):
                raise PlatformError("INVALID_RESPONSE", response.status) from None
            if not isinstance(value, dict):
                raise PlatformError("INVALID_RESPONSE", response.status)
            if response.status not in (200, 202):
                code = value.get("code", "HTTP_" + str(response.status))
                if not isinstance(code, str) or not code.replace("_", "").isalnum() or len(code) > 80:
                    code = "HTTP_" + str(response.status)
                identifier = value.get("request_id", "")
                if not isinstance(identifier, str) or len(identifier) > 128 or not all(c.isalnum() or c in "-_.:" for c in identifier):
                    identifier = ""
                error = PlatformError(code, response.status, identifier)
                error.retry_after = retry_after(response.getheader("Retry-After"))
                raise error
            return value
        except (OSError, TimeoutError, http.client.HTTPException):
            raise PlatformError("NETWORK_ERROR") from None
        finally:
            connection.close()

    def request(self, method, path, metadata=None, *, package=None, key=None, deadline=None):
        for attempt in range(4):
            try:
                return self._send(method, path, metadata, package, key, deadline)
            except PlatformError as error:
                retryable = error.code in ("NETWORK_ERROR", "UPLOAD_IN_PROGRESS") or error.status in (429, 502, 503, 504)
                if not retryable or attempt == 3 or (method != "GET" and key is None):
                    raise
                base = 2**attempt
                pause = max(base + random.uniform(0, base), getattr(error, "retry_after", 0))
                if deadline is not None and time.monotonic() + pause >= deadline:
                    raise
                time.sleep(pause)

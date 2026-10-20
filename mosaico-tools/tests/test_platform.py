from __future__ import annotations

import ast
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
import urllib.error
from contextlib import ExitStack, redirect_stderr, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from mosaico_cli.cli import build_parser, main
from mosaico_cli.errors import SelectionError
from mosaico_cli.platform_account import account
from mosaico_cli.platform_client import (
    PlatformClient,
    PlatformError,
    credential,
    private_root,
    server_url,
    write_state,
)


class PlatformTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        # Windows temporary paths may use 8.3 aliases; production resolves paths.
        self.root = Path(self.temporary.name).resolve()
        patch = mock.patch.dict(
            os.environ,
            {
                "XDG_STATE_HOME": str(self.root / "state"),
                "LOCALAPPDATA": str(self.root / "state"),
            },
            clear=True,
        )
        patch.start()
        self.addCleanup(patch.stop)
        # Keep home lookup valid after clearing the environment, including on
        # Windows, and isolate macOS state (which uses the home directory).
        home_patch = mock.patch("pathlib.Path.home", return_value=self.root)
        home_patch.start()
        self.addCleanup(home_patch.stop)
        self.manifest = {
            "schema": "mosaico-ideas/project-upload/v1",
            "project": {"title": "Demo", "categories": ["sensors"], "license": "MIT"},
            "content": {
                "readme": "README.md",
                "covers": ["cover.png"],
                "attachments": ["circuit.pdf"],
            },
            "release": {"changelog": "Fixed reconnect"},
        }
        (self.root / "README.md").write_text(
            "![cover](cover.png)\n[doc][c]\n[c]: circuit.pdf\n[site](https://example.com)\n`![code](missing.png)`\n",
            encoding="utf-8",
        )
        (self.root / "cover.png").write_bytes(b"cover")
        (self.root / "circuit.pdf").write_bytes(b"pdf")
        self.write_manifest()

    def write_manifest(self):
        (self.root / "mosaico-ideas.json").write_text(
            json.dumps(self.manifest), encoding="utf-8"
        )

    def args(self, **updates):
        values = {
            "server": "https://ideas.test",
            "json": True,
            "create": True,
            "update": None,
            "yes": True,
            "bundle": str(self.root / "app.irisfw"),
            "skip_build": False,
            "project": str(self.root),
            "release_version": None,
        }
        values.update(updates)
        return SimpleNamespace(**values)

    def plan(self):
        return {
            "release": "1.2.3",
            "chip_id": 32,
            "flash_size": 16 * 1024 * 1024,
            "signature_verified": False,
            "key_id": None,
            "components": [
                {
                    "kind": "application",
                    "flags": 0,
                    "target_offset": 0x10000,
                    "size": 32,
                }
            ],
        }


    def test_account_without_workspace(self):
        with ExitStack() as stack:
            load = stack.enter_context(mock.patch("mosaico_cli.cli.load_workspace"))
            stack.enter_context(
                mock.patch(
                    "mosaico_cli.platform_account.account",
                    return_value={"status": "ok"},
                )
            )
            stack.enter_context(redirect_stdout(io.StringIO()))
            self.assertEqual(
                main(["account", "status", "--server", "https://ideas.test", "--json"]),
                0,
            )
        load.assert_not_called()













    def test_private_state_is_isolated_on_all_host_platforms(self):
        for platform in ("windows", "macos", "linux"):
            with self.subTest(platform=platform), mock.patch(
                "mosaico_cli.host.host_platform", return_value=platform
            ):
                path = private_root("https://ideas.test")
                self.assertTrue(path.is_dir())
                path.resolve().relative_to(self.root)

    def test_private_credentials_and_environment_priority(self):
        path = private_root("https://ideas.test") / "credential.json"
        write_state(path, {"access_token": "stored"})
        self.assertEqual(credential("https://ideas.test"), "stored")
        if os.name != "nt":
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        with mock.patch.dict(os.environ, {"MAKER_SPARK_TOKEN": "environment"}):
            self.assertEqual(credential("https://ideas.test"), "environment")

    def test_private_state_never_imports_old_credentials_or_upload_ledgers(self):
        server = "https://ideas.test"
        state = self.root / "private"
        legacy = state / "maker-spark" / hashlib.sha256(server.encode()).hexdigest()
        legacy.mkdir(parents=True)
        write_state(legacy / "credential.json", {"access_token": "existing"})
        write_state(legacy / "upload-retry.json", {"key": "original-idempotency-key"})
        with mock.patch("mosaico_cli.platform_client.state_root", return_value=state):
            with self.assertRaises(SelectionError):
                credential(server)
            current = private_root(server)
            self.assertEqual(current.parent.name, "mosaico-ideas")
            self.assertEqual(current.parent.parent.name, "0.2")
            self.assertTrue(legacy.exists())
            self.assertFalse((current / "credential.json").exists())
            self.assertFalse((current / "upload-retry.json").exists())
            self.assertEqual(json.loads((legacy / "upload-retry.json").read_text()),
                             {"key": "original-idempotency-key"})
            self.assertEqual(private_root(server), current)
            if os.name != "nt":
                self.assertEqual(current.stat().st_mode & 0o777, 0o700)

    def test_existing_private_state_is_not_overwritten_by_legacy_state(self):
        server = "https://ideas.test"
        state = self.root / "private"
        legacy = state / "maker-spark" / hashlib.sha256(server.encode()).hexdigest()
        with mock.patch("mosaico_cli.platform_client.state_root", return_value=state):
            current = private_root(server)
            write_state(current / "credential.json", {"access_token": "current"})
            legacy.mkdir(parents=True)
            write_state(legacy / "credential.json", {"access_token": "stale"})
            self.assertEqual(credential(server), "current")
            self.assertTrue((legacy / "credential.json").exists())

    def test_server_selection_for_upload_and_account_commands(self):
        commands = (
            ["project", "upload", "release.zip"],
            ["account", "login"],
            ["account", "status"],
            ["account", "logout"],
        )
        cases = (
            (None, None, None, "https://mosaico.ai"),
            (None, None, "https://legacy.test/", "https://legacy.test"),
            (None, "https://new.test/", None, "https://new.test"),
            (None, "https://new.test", "https://legacy.test", "https://new.test"),
            ("https://explicit.test/", "https://new.test", "https://legacy.test", "https://explicit.test"),
            (None, "", "https://legacy.test", "https://legacy.test"),
            (None, "", "", "https://mosaico.ai"),
        )
        for command in commands:
            for explicit, current, legacy, expected in cases:
                environment = {}
                if current is not None:
                    environment["MOSAICO_IDEAS_SERVER"] = current
                if legacy is not None:
                    environment["MAKER_SPARK_SERVER"] = legacy
                argv = command + (["--server", explicit] if explicit else [])
                with self.subTest(argv=argv, environment=environment), mock.patch.dict(
                    os.environ, environment, clear=True
                ):
                    args = build_parser().parse_args(argv)
                    self.assertEqual(server_url(args.server), expected)

    def test_invalid_environment_server_does_not_fall_back(self):
        for name in ("MOSAICO_IDEAS_SERVER", "MAKER_SPARK_SERVER"):
            with self.subTest(name=name), mock.patch.dict(os.environ, {name: "http://remote.test"}):
                with self.assertRaises(SelectionError):
                    server_url(None)
                self.assertEqual(server_url("https://explicit.test"), "https://explicit.test")

    def test_public_upload_help_uses_new_manifest_name(self):
        output = io.StringIO()
        with redirect_stdout(output), self.assertRaises(SystemExit) as error:
            build_parser().parse_args(["project", "upload", "--help"])
        self.assertEqual(error.exception.code, 0)
        self.assertIn("ZIP", output.getvalue())


    def test_server_validation(self):
        for value in (
            "http://example.com",
            "https://user:pass@example.com",
            "https://example.com/?token=secret",
            "https://example.com/path",
        ):
            with self.subTest(value=value), self.assertRaises(SelectionError):
                server_url(value)
        self.assertEqual(server_url("http://localhost:3000/"), "http://localhost:3000")

    def test_transport_sanitizes_errors(self):
        client = PlatformClient("https://ideas.test", "secret-token")
        client.opener.open = mock.Mock(
            side_effect=urllib.error.HTTPError(
                "https://signed?secret",
                403,
                "secret",
                {},
                io.BytesIO(
                    json.dumps(
                        {
                            "code": "DENIED",
                            "message": "secret-token",
                            "request_id": "req-123",
                        }
                    ).encode()
                ),
            )
        )
        with self.assertRaises(PlatformError) as raised:
            client.request("POST", "/something")
        self.assertNotIn("secret", str(raised.exception))
        self.assertIn("req-123", str(raised.exception))

    def test_storage_put_does_not_leak_token_and_412_completes(self):
        client = PlatformClient("https://ideas.test", "private-token")
        client.opener.open = mock.Mock(
            side_effect=urllib.error.HTTPError(
                "https://store.test?secret", 412, "exists", {}, io.BytesIO(b"")
            )
        )
        client.put_object(
            {
                "upload_url": "https://store.test/key?secret",
                "headers": {"If-None-Match": "*"},
            },
            b"data",
        )
        request = client.opener.open.call_args.args[0]
        self.assertIsNone(request.get_header("Authorization"))

    def test_login_poll_and_json_do_not_disclose_token(self):
        client = mock.Mock()
        client.request.side_effect = [
            {
                "verification_uri": "https://ideas.test/auth/device",
                "user_code": "1234",
                "device_code": "device-secret",
                "expires_in": 600,
                "interval": 1,
            },
            PlatformError("AUTHORIZATION_PENDING", 400),
            {"access_token": "token-secret", "expires_in": 3600},
        ]
        stderr = io.StringIO()
        with ExitStack() as stack:
            stack.enter_context(
                mock.patch(
                    "mosaico_cli.platform_account.PlatformClient", return_value=client
                )
            )
            stack.enter_context(mock.patch("mosaico_cli.platform_account.time.sleep"))
            stack.enter_context(redirect_stderr(stderr))
            result = account(self.args(account_action="login", open_browser=False))
        self.assertNotIn("secret", json.dumps(result) + stderr.getvalue())
        self.assertIn("1234", stderr.getvalue())
        self.assertEqual(credential("https://ideas.test"), "token-secret")



    def test_python38_syntax(self):
        tools = Path(__file__).resolve().parents[1] / "tools/mosaico_cli"
        for path in tools.glob("platform_*.py"):
            ast.parse(path.read_text(), feature_version=(3, 8))



    def test_login_rejects_malformed_response(self):
        client = mock.Mock()
        for response in (
            {},
            {
                "verification_uri": "http://remote.test/device",
                "device_code": "secret",
                "user_code": "1234",
                "expires_in": 600,
            },
        ):
            client.request.return_value = response
            with ExitStack() as stack:
                stack.enter_context(
                    mock.patch(
                        "mosaico_cli.platform_account.PlatformClient",
                        return_value=client,
                    )
                )
                stack.enter_context(self.assertRaises(PlatformError))
                account(self.args(account_action="login", open_browser=False))





    def test_json_error_retains_request_id_without_credentials(self):
        stderr = io.StringIO()
        with mock.patch(
            "mosaico_cli.platform_account.account",
            side_effect=PlatformError("DENIED", 403, "request-123"),
        ), redirect_stderr(stderr):
            status = main(
                ["account", "status", "--server", "https://ideas.test", "--json"]
            )
        result = json.loads(stderr.getvalue())
        self.assertEqual(status, 5)
        self.assertEqual(result["error"], "platform_error")
        self.assertEqual(result["details"]["request_id"], "request-123")
        self.assertNotIn("Authorization", stderr.getvalue())
        self.assertNotIn("access_token", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()

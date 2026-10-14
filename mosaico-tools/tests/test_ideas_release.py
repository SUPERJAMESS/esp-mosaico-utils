"""Real ZIP validators and HTTP/SQLite protocol round-trips, without hardware."""
import copy
import hashlib
import http.client
import io
import json
from pathlib import Path
import socket
import struct
import sys
import threading
import time
import uuid
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "mosaico-tools/tools"))
sys.path.insert(0, str(ROOT / "esp-mosaico-vibe-mode/tests/fixtures"))
from ideas_release import archive, descriptor, encoded, png, ref, upload
from mosaico_cli.ideas_content import ContentError, read_archive, strict_json
from mosaico_cli.ideas_release import validate_release
from mosaico_cli.ideas_test_server import IdeasServer
from mosaico_cli.ideas_test_store import Store
from mosaico_cli.ideas_transport import ReleaseClient, retry_after
from mosaico_cli.platform_client import PlatformError
from mosaico_cli.platform_upload import upload_archive


def test_full_closure_preserves_every_original_byte():
    _, outer, raw = upload()
    release = validate_release(raw)
    manifest, files = release.device_release("app-one", "042731", "release-one")
    assert manifest["schema"] == "mosaico-ideas/device-release/1"
    for path, data in files.items():
        if path.startswith("device/"): assert data == outer[path]
    inner = read_archive(outer["firmware/application.irisfw"])
    assert files["firmware/manifest.json"] == inner["manifest.json"]
    for component in release.firmware["components"]:
        assert files["firmware/" + component["file"]] == inner[component["file"]]


@pytest.mark.parametrize("kind", ["extra", "missing", "hash", "size", "locale", "icon-width", "icon-rgb", "glyph", "font", "external-image", "reference", "firmware", "protected"])
def test_package_semantic_rejection(kind):
    manifest, files, _ = upload()
    view = manifest["device"]["presentations"][1]
    if kind == "extra": files["extra.txt"] = b"unlisted"
    if kind == "missing": files.pop("device/en/font.miff")
    if kind == "hash": files["device/en/font.miff"] += b"changed"
    if kind == "size": view["assets"][0]["size"] += 1
    if kind in ("locale", "glyph", "reference"):
        display = json.loads(files["device/en/display.json"])
        if kind == "locale": display["locale"] = "fr"
        if kind == "glyph": display["name"]["glyphs"][0][0] = 999
        if kind == "reference": display["name"]["font"] = "../zh-CN/font.miff"
        files["device/en/display.json"] = encoded(display)
        view["display"] = ref("device/en/display.json", files["device/en/display.json"])
    if kind in ("icon-width", "icon-rgb", "font"):
        path = "device/en/font.miff" if kind == "font" else "device/en/icon.png"
        data = b"bad-font" if kind == "font" else png(63)
        if kind == "icon-rgb":
            from PIL import Image
            output = io.BytesIO(); Image.new("RGB", (64, 64)).save(output, format="PNG"); data = output.getvalue()
        files[path] = data
        view["assets"] = [ref(a["path"], files[a["path"]]) for a in view["assets"]]
    if kind == "external-image":
        files["website/readme.md"] = b"![external](https://example.com/image.png)"
        manifest["website"]["readme"] = ref("website/readme.md", files["website/readme.md"])
    if kind in ("firmware", "protected"):
        inner = read_archive(files["firmware/application.irisfw"])
        firmware = json.loads(inner["manifest.json"])
        if kind == "firmware": inner["app.bin"] = b"not-an-app"
        else: firmware["components"][1]["target_offset"] = 0x20000
        inner["manifest.json"] = encoded(firmware)
        files["firmware/application.irisfw"] = archive(inner)
        manifest["firmware"] = ref("firmware/application.irisfw", files["firmware/application.irisfw"])
    files["manifest.json"] = encoded(manifest)
    with pytest.raises(ContentError): validate_release(archive(files))


@pytest.mark.parametrize("name", ["../x", "/x", "a/../b", "a%20b", "a\\b", "a/b/c/d/e", "a"*65, "中文"])
def test_zip_paths_are_checked_before_extraction(name):
    with pytest.raises(ContentError): read_archive(archive({name:b"content"}))


def test_zip_case_collisions_and_links():
    with pytest.raises(ContentError): read_archive(archive({"a":b"one", "A":b"two"}))
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w") as z:
        info = zipfile.ZipInfo("link"); info.create_system = 3; info.external_attr = 0o120777 << 16
        z.writestr(info, b"../target")
    with pytest.raises(ContentError): read_archive(data.getvalue())


def test_zip_byte_budgets_and_directory_parents():
    with pytest.raises(ContentError): read_archive(archive({"big":b"x"*1000}), limit=500)
    with pytest.raises(ContentError): read_archive(archive({"empty/":b""}))
    assert read_archive(archive({"a/":b"", "a/file":b"x"})) == {"a/file":b"x"}


@pytest.mark.parametrize("raw", [b'{"x":1,"x":2}', b'{"x":NaN}', b'{"x":"\\u0000"}', b'{"x":"\\ud800"}', b'{"x":"\xff"}', b'[] trailing', b'['*20+b'0'+b']'*20])
def test_json_has_no_ambiguous_decodings(raw):
    with pytest.raises(ContentError): strict_json(raw)


@pytest.fixture
def server(tmp_path):
    store = Store(tmp_path / "state")
    server = IdeasServer(("127.0.0.1", 0), store, "fixture-token")
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    yield server, ReleaseClient("http://127.0.0.1:" + str(server.server_port), "fixture-token"), store
    server.shutdown(); server.server_close(); thread.join()


def publish_draft(client, directory, version="1.0.0", mode="create", **target):
    _, _, raw = upload(version)
    path = directory / (uuid.uuid4().hex + ".zip"); path.write_bytes(raw)
    request = dict(mode=mode, archive=dict(size=len(raw), sha256=hashlib.sha256(raw).hexdigest()), **target)
    result = upload_archive(client, path, request, directory / (uuid.uuid4().hex + ".json"), wait_seconds=10, poll_seconds=0.005)
    return result, request, path


def get(server, path):
    c = http.client.HTTPConnection("127.0.0.1", server.server_port)
    c.request("GET", path, headers={"If-None-Match":"*"})
    response = c.getresponse(); data = response.read(); status = response.status; headers = dict(response.getheaders()); c.close()
    return status, headers, data


CAPS = "?profile=esp-mosaico%2Fv2&chip_id=32&flash_size=16777216&recovery_version=0.2.0"


def test_draft_publication_immutable_get_and_bilingual_download(server, tmp_path):
    http, client, store = server
    result, _, _ = publish_draft(client, tmp_path)
    latest = "/api/v1/ideas/" + result["code"] + "/manifest.json"
    assert result["status"] == "draft" and get(http, latest + CAPS)[0] == 404
    store.publish(result["release_id"])
    status, headers, raw = get(http, latest + CAPS)
    assert status == 200 and headers["Cache-Control"] == "no-store"
    assert headers["ETag"] == '"' + hashlib.sha256(raw).hexdigest() + '"'
    fixed = latest.replace("/manifest.json", "/releases/" + result["release_id"] + "/manifest.json")
    assert get(http, fixed)[2] == raw
    root = json.loads(raw)
    refs = [root["firmware"]["manifest"]]
    for p in root["device"]["presentations"]: refs += [p["display"]] + p["assets"]
    for f in refs:
        status, headers, content = get(http, "/api/v1/ideas/files/" + f["sha256"])
        assert status == 200 and len(content) == f["size"] and hashlib.sha256(content).hexdigest() == f["sha256"]
    assert get(http, latest)[0] == 400
    assert get(http, latest + CAPS + "&chip_id=32")[0] == 400
    assert get(http, latest + CAPS.replace("16777216", "8388608"))[0] == 409
    assert get(http, fixed + CAPS)[0] == 400


def test_upload_idempotency_and_cas_no_duplicate_drafts(server, tmp_path):
    http, client, store = server
    result, request, package = publish_draft(client, tmp_path)
    key = uuid.uuid4().hex
    first = client.request("POST", "/ideas/uploads", request, package=package, key=key)
    for _ in range(100):
        state = client.request("GET", "/ideas/uploads/" + first["upload_id"])
        if state["status"] == "complete": break
        time.sleep(.005)
    again = client.request("POST", "/ideas/uploads", request, package=package, key=key)
    assert again["upload_id"] == first["upload_id"]
    _, _, replacement = upload("9.0.0")
    changed = copy.deepcopy(request); changed["archive"] = dict(size=len(replacement), sha256=hashlib.sha256(replacement).hexdigest())
    package.write_bytes(replacement)
    with pytest.raises(PlatformError) as error: client.request("POST", "/ideas/uploads", changed, package=package, key=key)
    assert error.value.code == "IDEMPOTENCY_CONFLICT"
    update, _, _ = publish_draft(client, tmp_path, "2.0.0", "update", application_id=result["application_id"], expected_project_version=1)
    assert update["code"] == result["code"] and update["project_version"] == 2
    with pytest.raises(PlatformError) as error:
        publish_draft(client, tmp_path, "3.0.0", "update", application_id=result["application_id"], expected_project_version=1)
    assert error.value.code == "PROJECT_VERSION_CONFLICT"


def test_resume_receipt_survives_server_restart(tmp_path):
    store = Store(tmp_path / "state")
    _,_,raw = upload(); request = dict(mode="create", archive=dict(size=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
    identifier, duplicate = store.begin("owner", "fixed-key", request)
    assert not duplicate
    store.received(identifier, raw)
    store = Store(tmp_path / "state")
    assert store.pending() == [identifier]
    store.validate(identifier)
    receipt = store.status(identifier, "owner")
    assert receipt["status"] == "complete"
    assert store.begin("owner", "fixed-key", request) == (identifier, True)
    assert Store(tmp_path / "state").status(identifier, "owner") == receipt
    with pytest.raises(ContentError): store.status(identifier, "another-owner")


def test_expired_failed_receipt_remains_a_tombstone(tmp_path):
    store = Store(tmp_path)
    identifier, _ = store.begin("owner", "key", dict(mode="create", archive=dict(size=1, sha256="a"*64)))
    with store.db() as db: db.execute("UPDATE uploads SET state='failed',updated=0 WHERE id=?", (identifier,))
    with pytest.raises(ContentError, match="expired"): store.status(identifier, "owner")
    with store.db() as db: assert db.execute("SELECT state FROM uploads WHERE id=?", (identifier,)).fetchone()[0] == "expired"


def test_retry_after_seconds_and_date():
    from email.utils import formatdate
    assert retry_after("15") == 15
    assert 0 < retry_after(formatdate(time.time() + 30, usegmt=True)) <= 30
    assert retry_after("nonsense") == 0


def test_prebuilt_packaging_is_deterministic_and_preserves_display_bytes(tmp_path):
    from mosaico_cli.ideas_archive import package_project
    root, files, _ = upload()
    for name, raw in files.items():
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw)
    config = dict(schema="mosaico-ideas/project-release/1", project=root["project"],
                  firmware=root["firmware"]["path"], website=dict(readme="website/readme.md"),
                  device=dict(default_locale="zh-CN", presentations={
                      locale: dict(mode="prebuilt", display="device/"+locale+"/display.json") for locale in ("zh-CN", "en")}))
    (tmp_path / "mosaico-ideas.json").write_bytes(encoded(config))
    one, two = tmp_path / "one.zip", tmp_path / "two.zip"
    assert package_project(tmp_path, one)["sha256"] == package_project(tmp_path, two)["sha256"]
    assert one.read_bytes() == two.read_bytes()
    result = validate_release(one)
    for locale in ("zh-CN", "en"):
        path = "device/"+locale+"/display.json"
        assert result.files[path] == files[path]
    config["device"]["presentations"]["en"]["name"] = "Mixed input is forbidden"
    (tmp_path / "mosaico-ideas.json").write_bytes(encoded(config))
    with pytest.raises(ContentError): package_project(tmp_path, two)
    assert one.read_bytes() == two.read_bytes(), "Failed packaging must preserve an existing archive"


def test_account_identity_survives_token_rotation_and_restart(server, tmp_path):
    http, client, store = server
    owner = client.request("GET", "/auth/cli/token")["user"]["id"]
    result, request, package = publish_draft(client, tmp_path)
    http.token = "rotated-token"
    rotated = ReleaseClient(client.server, "rotated-token")
    assert rotated.request("GET", "/auth/cli/token")["user"]["id"] == owner
    with pytest.raises(PlatformError) as error: client.request("GET", "/auth/cli/token")
    assert error.value.status == 401
    update, _, _ = publish_draft(rotated, tmp_path, mode="update",
        application_id=result["application_id"], expected_project_version=1)
    assert update["code"] == result["code"] and update["project_version"] == 2
    assert Store(store.root).account_id("a-new-token-hash") == owner

"""Immutable Ideas ZIP validation and derivation of directory device releases."""
from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path, PurePosixPath
import posixpath
from urllib.parse import unquote, urlsplit

from .ideas_content import (ARCHIVE_BYTES, MIB, ContentError, descriptor, digest,
                            encoded, image_check, read_archive, require, strict_json, validate_paths)
from .ideas_display import validate_display
from .ideas_firmware import validate_firmware

DOCS = Path(__file__).resolve().parents[2] / "docs/ideas-file-release"


def validate_schema(document, name):
    from jsonschema import Draft202012Validator
    schema = json.loads((DOCS / name).read_text(encoding="utf-8"))
    errors = list(Draft202012Validator(schema).iter_errors(document))
    require(not errors, "Manifest schema validation failed" + (": " + errors[0].message if errors else ""))


def file_references(manifest):
    yield manifest["firmware"]
    website = manifest["website"]
    for name in ("readme", "changelog"):
        if name in website: yield website[name]
    for name in ("covers", "resources", "attachments"):
        yield from website.get(name, [])
    for presentation in manifest["device"]["presentations"]:
        yield presentation["display"]
        yield from presentation["assets"]


def check_reference(ref, files):
    path = ref["path"]
    require(path in files, "Missing declared file", path, "MISSING_FILES")
    require(len(files[path]) == ref["size"], "File size mismatch", path, "FILE_SIZE_MISMATCH")
    require(digest(files[path]) == ref["sha256"], "File SHA-256 mismatch", path, "FILE_HASH_MISMATCH")


def validate_website(website, files):
    from markdown_it import MarkdownIt
    from html.parser import HTMLParser
    referenced = set()
    def destination(raw, source, embedded=False):
        url = urlsplit(raw)
        if url.scheme or url.netloc:
            require(not embedded and url.scheme in ("http", "https") and url.netloc,
                    "Embedded resources must be included in the ZIP", source)
            return
        if not url.path:
            require(bool(url.fragment) and not embedded, "Empty resource reference", source)
            return
        require(not url.query and not raw.startswith("/") and "\\" not in raw,
                "Invalid Markdown resource path", source)
        decoded = unquote(url.path)
        require(decoded == url.path, "Encoded resource paths are forbidden", source)
        path = posixpath.normpath(posixpath.join(str(PurePosixPath(source).parent), decoded))
        require(not path.startswith("../") and path in files and path != "manifest.json",
                "Undeclared or escaping Markdown resource", path, "MISSING_FILES")
        referenced.add(path)
    class HtmlLinks(HTMLParser):
        def handle_starttag(self, tag, attrs):
            for key, value in attrs:
                if key in ("src", "href", "poster") and value:
                    destination(value, current, embedded=tag != "a")
                require(key != "srcset", "Use an explicit local image source", current)
        handle_startendtag = handle_starttag
    for field, chars, limit in (("readme", 100000, 400000), ("changelog", 10000, 40000)):
        if field not in website: continue
        current = website[field]["path"]
        try: text = files[current].decode("utf-8")
        except UnicodeError as error: raise ContentError("INVALID_MANIFEST", "Markdown must be UTF-8", current) from error
        require(0 < len(text) <= chars and len(files[current]) <= limit and not text.startswith("\ufeff") and "\0" not in text,
                "Markdown exceeds text limits", current)
        def tokens(values):
            for token in values:
                if token.type == "image": destination(token.attrGet("src") or "", current, True)
                elif token.type == "link_open": destination(token.attrGet("href") or "", current)
                elif token.type in ("html_block", "html_inline"): HtmlLinks().feed(token.content)
                if token.children: tokens(token.children)
        tokens(MarkdownIt("commonmark").parse(text))
    allowed = {website["readme"]["path"]}
    if "changelog" in website: allowed.add(website["changelog"]["path"])
    for group in ("covers", "resources", "attachments"):
        for ref in website.get(group, []):
            path = ref["path"]
            allowed.add(path)
            data = files[path]
            is_image = PurePosixPath(path).suffix.lower() in (".jpg", ".jpeg", ".png", ".gif", ".webp") or data.startswith((b"\x89PNG", b"\xff\xd8", b"GIF87a", b"GIF89a")) or data[:4] == b"RIFF" and data[8:12] == b"WEBP"
            if group == "covers" or is_image: image_check(data, path)
            require(len(data) <= (10 if is_image or group == "covers" else 20)*MIB,
                    "Website resource exceeds limit", path)
            if group == "resources": require(path in referenced, "Unused website resource", path)
    require(referenced <= allowed, "Markdown must reference declared website files")


@dataclass
class Release:
    manifest: dict
    files: dict
    firmware: dict
    firmware_files: dict
    displays: dict
    archive_sha256: str

    def device_release(self, application_id, code, release_id):
        output = {"firmware/" + name: data for name, data in self.firmware_files.items()}
        for presentation in self.manifest["device"]["presentations"]:
            for ref in [presentation["display"], *presentation["assets"]]:
                output[ref["path"]] = self.files[ref["path"]]
        root = {"schema": "mosaico-ideas/device-release/1", "release_id": release_id,
                "application": {"id": application_id, "code": code},
                "firmware": {"manifest": descriptor("firmware/manifest.json", output["firmware/manifest.json"])},
                "device": self.manifest["device"]}
        validate_schema(root, "download-manifest.schema.json")
        output["manifest.json"] = encoded(root)
        validate_paths(output)
        require(len(output) <= 32 and len(output["manifest.json"]) <= 65536
                and sum(map(len, output.values())) <= 32*MIB,
                "Device collection exceeds its total capacity", code="PACKAGE_TOO_LARGE")
        return root, output


def validate_release(source):
    if isinstance(source, bytes): raw = source
    else:
        with Path(source).open("rb") as stream: raw = stream.read(ARCHIVE_BYTES+1)
    files = read_archive(raw)
    require("manifest.json" in files, "Root manifest.json is required", code="MISSING_FILES")
    manifest = strict_json(files["manifest.json"])
    validate_schema(manifest, "upload-manifest.schema.json")
    refs = list(file_references(manifest))
    validate_paths(["manifest.json"] + [ref["path"] for ref in refs])
    require(set(files) == {"manifest.json", *(ref["path"] for ref in refs)},
            "ZIP and declared file collection differ", code="MISSING_FILES")
    for ref in refs: check_reference(ref, files)
    validate_website(manifest["website"], files)
    try:
        firmware, members = validate_firmware(files[manifest["firmware"]["path"]])
    except (ValueError, TypeError) as error:
        raise ContentError("INVALID_FIRMWARE", str(error), manifest["firmware"]["path"]) from error
    displays = {p["display"]["path"].split("/")[1]: validate_display(p, files)
                for p in manifest["device"]["presentations"]}
    release = Release(manifest, files, firmware, members, displays, digest(raw))
    # Reserve worst-case identity lengths before accepting a source ZIP.
    release.device_release("a"*64, "000000", "r"*64)
    return release

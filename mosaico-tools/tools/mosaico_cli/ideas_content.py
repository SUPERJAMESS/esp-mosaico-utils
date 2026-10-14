"""Bounded primitives shared by the Ideas publisher and reference server."""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import re
import stat
import struct
import zipfile
import zlib

from .errors import SelectionError

MIB = 1024 * 1024
ARCHIVE_BYTES = 100 * MIB
MANIFEST_BYTES = 65536
PATH = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}(?:/[A-Za-z0-9][A-Za-z0-9._-]{0,63}){0,3}\Z")


class ContentError(SelectionError):
    def __init__(self, code: str, message: str, path: str = ""):
        super().__init__(message, details={"code": code, "path": path})
        self.code, self.path = code, path


def require(condition, message, path="", code="INVALID_MANIFEST"):
    if not condition:
        raise ContentError(code, message, path)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def descriptor(path, data):
    return {"path": path, "size": len(data), "sha256": digest(data)}


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")


def strict_json(raw, limit=MANIFEST_BYTES, depth=12, path="manifest.json"):
    require(0 < len(raw) <= limit, "JSON exceeds its byte limit", path)
    def pairs(items):
        out = {}
        for key, value in items:
            require(key not in out, "Duplicate JSON key", path)
            out[key] = value
        return out
    try:
        text = raw.decode("utf-8")
        level, quoted, escaped = 0, False, False
        for char in text:
            if quoted:
                if escaped: escaped = False
                elif char == "\\": escaped = True
                elif char == '"': quoted = False
            elif char == '"': quoted = True
            elif char in "[{":
                level += 1
                require(level <= depth, "JSON nesting limit exceeded", path)
            elif char in "]}": level -= 1
        value = json.loads(text, object_pairs_hook=pairs,
                           parse_constant=lambda _: require(False, "Non-finite JSON number", path))
        def strings(node):
            if isinstance(node, str):
                node.encode("utf-8")
                require("\0" not in node, "NUL in JSON string", path)
            elif isinstance(node, dict):
                for k, v in node.items(): strings(k); strings(v)
            elif isinstance(node, list):
                for v in node: strings(v)
        strings(value)
        require(isinstance(value, dict), "JSON root must be an object", path)
        return value
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ContentError("INVALID_MANIFEST", "Invalid UTF-8 JSON", path) from error


def path_valid(path):
    return isinstance(path, str) and len(path) <= 192 and PATH.fullmatch(path) is not None


def validate_paths(paths):
    seen = set()
    for path in paths:
        require(path_valid(path), "Invalid release-relative path", str(path))
        key = path.lower()
        require(key not in seen, "Duplicate/case-folded path", path)
        require(not any(key.startswith(p + "/") or p.startswith(key + "/") for p in seen),
                "File/directory path collision", path)
        seen.add(key)


def zip_extra(extra, name):
    position = 0
    while position < len(extra):
        require(position + 4 <= len(extra), "Malformed ZIP extra", name, "INVALID_ARCHIVE")
        tag, size = struct.unpack_from("<HH", extra, position)
        require(tag != 1 and position + 4 + size <= len(extra), "ZIP64 or malformed ZIP extra", name, "INVALID_ARCHIVE")
        value = extra[position + 4:position + 4 + size]
        if tag == 0x7075:
            require(len(value) >= 5 and value[0] == 1 and
                    int.from_bytes(value[1:5], "little") == zlib.crc32(name.encode("ascii")) and
                    value[5:] == name.encode("ascii"), "Inconsistent Unicode ZIP path", name, "INVALID_ARCHIVE")
        position += 4 + size


def read_archive(source, *, limit=ARCHIVE_BYTES, files=164, entries=512, directories=True):
    """Read bounded ordinary members; never extract untrusted paths to disk."""
    if isinstance(source, bytes):
        data = source
    else:
        with Path(source).open("rb") as stream:
            data = stream.read(limit + 1)
    require(0 < len(data) <= limit, "ZIP exceeds byte limit", code="ARCHIVE_TOO_LARGE")
    end = data.rfind(b"PK\x05\x06", max(0, len(data) - 65557))
    require(end >= 0 and end + 22 <= len(data), "Missing ZIP end record", code="INVALID_ARCHIVE")
    _, disk, cd_disk, disk_count, count, cd_size, cd_offset, comment = struct.unpack_from("<4s4H2IH", data, end)
    require(disk == cd_disk == 0 and disk_count == count and count < 65535
            and end + 22 + comment == len(data) and cd_offset + cd_size == end,
            "Split, ZIP64, or inconsistent ZIP directory", code="INVALID_ARCHIVE")
    result, dirs, total = {}, [], 0
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            members = archive.infolist()
            require(0 < len(members) <= entries and len(members) == count,
                    "ZIP member limit exceeded", code="INVALID_ARCHIVE")
            seen = set()
            for item in members:
                name = item.orig_filename
                is_dir = item.is_dir()
                key = name[:-1] if is_dir else name
                require(path_valid(key) and name == item.filename and key.lower() not in seen,
                        "Invalid or duplicate ZIP path", name, "INVALID_ARCHIVE")
                seen.add(key.lower())
                mode = item.external_attr >> 16
                kind = stat.S_IFMT(mode)
                require(kind in ((0, stat.S_IFDIR) if is_dir else (0, stat.S_IFREG)),
                        "ZIP links and special files are forbidden", name, "INVALID_ARCHIVE")
                require(not item.flag_bits & (1 | 0x40) and item.compress_type in (0, 8),
                        "Encrypted or unsupported ZIP compression", name, "INVALID_ARCHIVE")
                zip_extra(item.extra, name)
                offset = item.header_offset
                require(offset >= 0 and offset + 30 <= cd_offset and data[offset:offset+4] == b"PK\x03\x04",
                        "Invalid ZIP local header", name, "INVALID_ARCHIVE")
                header = struct.unpack_from("<4s5H3I2H", data, offset)
                require(header[2] == item.flag_bits and header[3] == item.compress_type,
                        "Local/central ZIP flags disagree", name, "INVALID_ARCHIVE")
                if not item.flag_bits & 8:
                    require(header[6:9] == (item.CRC, item.compress_size, item.file_size),
                            "Local/central ZIP sizes disagree", name, "INVALID_ARCHIVE")
                local_extra = data[offset+30+header[9]:offset+30+header[9]+header[10]]
                require(offset + 30 + header[9] + header[10] + item.compress_size <= cd_offset,
                        "ZIP member overlaps directory", name, "INVALID_ARCHIVE")
                zip_extra(local_extra, name)
                require(total + item.file_size <= limit, "ZIP expands beyond budget", name, "PACKAGE_TOO_LARGE")
                with archive.open(item) as stream:
                    content = stream.read(min(limit-total, item.file_size) + 1)
                    require(len(content) == item.file_size and not stream.read(1),
                            "ZIP member size mismatch", name, "INVALID_ARCHIVE")
                total += len(content)
                if is_dir:
                    require(directories and not content, "Invalid ZIP directory entry", name, "INVALID_ARCHIVE")
                    dirs.append(key)
                else:
                    result[name] = content
                    require(len(result) <= files, "Too many ordinary ZIP files", name, "INVALID_ARCHIVE")
            validate_paths(result)
            for directory in dirs:
                require(any(p.startswith(directory + "/") for p in result),
                        "Directory is not a declared file parent", directory, "INVALID_ARCHIVE")
    except (zipfile.BadZipFile, OSError, RuntimeError, EOFError, NotImplementedError, struct.error, zlib.error) as error:
        raise ContentError("INVALID_ARCHIVE", "Cannot decode ZIP archive") from error
    return result


def image_check(data, path, *, icon=False):
    from PIL import Image
    require(0 < len(data) <= (65536 if icon else 10*MIB), "Image byte limit exceeded", path, "INVALID_PRESENTATION")
    if icon:
        require(len(data) >= 33 and data[:8] == b"\x89PNG\r\n\x1a\n"
                and data[8:16] == b"\0\0\0\rIHDR"
                and data[16:29] == struct.pack(">IIBBBBB", 64, 64, 8, 6, 0, 0, 0),
                "Icon must be non-interlaced 64x64 8-bit RGBA PNG", path, "INVALID_PRESENTATION")
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        offset = 8
        while offset + 12 <= len(data):
            size = int.from_bytes(data[offset:offset+4], "big")
            tag = data[offset+4:offset+8]
            require(tag not in (b"acTL", b"fcTL", b"fdAT"), "APNG is not accepted", path, "INVALID_PRESENTATION")
            require(size <= len(data)-offset-12, "Truncated PNG chunk", path, "INVALID_PRESENTATION")
            offset += size + 12
            if tag == b"IEND": break
        require(offset == len(data), "Trailing or truncated PNG data", path, "INVALID_PRESENTATION")
    try:
        with Image.open(io.BytesIO(data)) as image:
            require(image.format in ("JPEG", "PNG", "GIF", "WEBP") and 0 < image.width <= 4096
                    and 0 < image.height <= 4096, "Unsupported image format or dimensions", path, "INVALID_PRESENTATION")
            require(getattr(image, "n_frames", 1) == 1 and not getattr(image, "is_animated", False),
                    "Animated images are not accepted", path, "INVALID_PRESENTATION")
            image.verify()
        with Image.open(io.BytesIO(data)) as image:
            image.load()
    except (OSError, ValueError, Image.DecompressionBombError) as error:
        raise ContentError("INVALID_PRESENTATION", "Image cannot be fully decoded", path) from error


def version_tuple(value):
    require(isinstance(value, str) and value.isascii() and len(value) <= 64,
            "Invalid Vibe Mode version")
    match = re.fullmatch(r"([0-9]+)\.([0-9]+)(?:\.([0-9]+))?(?:[-+].*)?", value)
    require(match is not None, "Invalid Vibe Mode version")
    parts = tuple(int(x or 0) for x in match.groups())
    require(all(x <= 0xffffffff for x in parts), "Version component overflow")
    return parts


def version_satisfies(running, minimum):
    current = version_tuple(running)
    if minimum is None:
        return True
    required = version_tuple(minimum)
    return current[0] == required[0] and current >= required

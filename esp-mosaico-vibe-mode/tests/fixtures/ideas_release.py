"""Synthetic protocol fixtures; no consuming workspace, credentials or device IO.

Images carry valid ESP header/checksum structure for validator tests only. They
are not executable firmware and must never be flashed to physical hardware.
"""
import hashlib
import io
import json
import struct
import zipfile
import zlib


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode()


def ref(path, data):
    return dict(path=path, size=len(data), sha256=hashlib.sha256(data).hexdigest())


def png(width=64, height=64):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress((b"\0" + b"\xff\x80\0\xff" * width) * height)) + chunk(b"IEND", b""))


def font():
    return (struct.pack("<4sHHIHHHHI", b"MIFF", 1, 24, 45, 1, 20, 24, 0, 24) +
            struct.pack("<IIHHhhi", 1, 44, 1, 1, 0, 1, 64) + b"\xff")


def image(version, segment_size=256):
    data = bytearray((32 + segment_size + 16) // 16 * 16)
    data[0] = 0xe9; data[1] = 1; data[23] = 1
    struct.pack_into("<H", data, 12, 32)
    struct.pack_into("<II", data, 24, 0x3f400020, segment_size)
    struct.pack_into("<I", data, 32, 0xabcd5432)
    data[48:48 + len(version)] = version.encode()
    data[80:87] = b"fixture"
    data[176:208] = b"\xab" * 32
    checksum = 0xef
    for value in data[32:32 + segment_size]: checksum ^= value
    data[-1] = checksum
    return bytes(data) + hashlib.sha256(data).digest()


def table():
    parts = [(1, 0, 0x9000, 0x2000, "otadata"), (1, 1, 0xb000, 0x1000, "phy_init"),
             (1, 2, 0xc000, 0x14000, "sysmeta"), (0, 0x20, 0x20000, 0x1c0000, "vibe_mode"),
             (1, 3, 0x1e0000, 0x20000, "coredump"), (0, 0x10, 0x210000, 0x500000, "main_app")]
    raw = b"".join(struct.pack("<HBBII16sI", 0x50aa, typ, sub, offset, size, name.encode(), 0)
                   for typ, sub, offset, size, name in parts)
    raw += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(raw).digest()
    return raw.ljust(4096, b"\xff")


def archive(files):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for path, data in files.items():
            info = zipfile.ZipInfo(path)
            # Malformed names must reach the validator unchanged on Windows.
            info.filename = path
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    return out.getvalue()


def descriptor(version="1.0.0", code="042731", application_bytes=256):
    fw = {"partition-table.bin": table(), "app.bin": image(version, application_bytes)}
    components = []
    for i, (path, kind, offset) in enumerate((("partition-table.bin", "partition_table", 0x8000), ("app.bin", "application", 0x210000))):
        f = ref(path, fw[path]); components.append(dict(id=i + 1, kind=kind, target_offset=offset, file=path, size=f["size"], sha256=f["sha256"]))
    manifest = dict(schema="esp-iris-system-update/0.2", release=version, minimum_recovery_version="0.2.0",
                    target=dict(chip_id=32, flash_size=16*1024*1024), target_layout_sha256=components[0]["sha256"], components=components)
    fw["manifest.json"] = encoded(manifest)
    files = {"firmware/" + path: data for path, data in fw.items()}
    views = []
    for locale in ("zh-CN", "en"):
        prefix = "device/" + locale + "/"
        text = "测试" if locale == "zh-CN" else "Test"
        block = dict(text=text, font="font.miff", width=20, height=20, glyphs=[[1, 0, 1]])
        display = dict(schema="mosaico-ideas/display/1", locale=locale, icon="icon.png", name=block, description=dict(block))
        files[prefix + "display.json"] = encoded(display)
        files[prefix + "font.miff"] = font(); files[prefix + "icon.png"] = png()
        views.append(dict(display=ref(prefix + "display.json", files[prefix + "display.json"]),
                          assets=[ref(prefix + name, files[prefix + name]) for name in ("font.miff", "icon.png")]))
    root = dict(schema="mosaico-ideas/device-release/1", release_id="release-" + version,
                application=dict(id="stable-app-id", code=code), firmware=dict(manifest=ref("firmware/manifest.json", fw["manifest.json"])),
                device=dict(profile="esp-mosaico/v2", default_locale="zh-CN", presentations=views))
    return root, files


def upload(version="1.0.0"):
    device, files = descriptor(version)
    fw = {path[len("firmware/"):]: data for path, data in files.items() if path.startswith("firmware/")}
    outer = {path: data for path, data in files.items() if path.startswith("device/")}
    outer["firmware/application.irisfw"] = archive(fw)
    outer["website/readme.md"] = "# 测试\nBilingual release.\n".encode()
    root = dict(schema="mosaico-ideas/release/1", project=dict(title="Test", categories=["display"], tags=["test"]),
                firmware=ref("firmware/application.irisfw", outer["firmware/application.irisfw"]),
                website=dict(readme=ref("website/readme.md", outer["website/readme.md"]), covers=[], resources=[], attachments=[]),
                device=device["device"])
    outer["manifest.json"] = encoded(root)
    return root, outer, archive(outer)

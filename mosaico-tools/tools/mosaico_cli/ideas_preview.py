"""One bounded multipart response containing a complete, immutable preview.

This is a transport representation, not another editable content manifest.
The root and every referenced file retain their exact published bytes.
"""
from .ideas_content import digest, require, strict_json

PREVIEW_BYTES = 65536 + 3072 + 1048576 + 4096


def preview_response(root, locale, read_blob):
    require(locale in ("zh-CN", "en"), "Invalid preview locale")
    manifest = strict_json(root, 65536)
    presentation = next(p for p in manifest["device"]["presentations"]
                        if p["display"]["path"] == f"device/{locale}/display.json")
    references = [manifest["firmware"]["manifest"], presentation["display"], *presentation["assets"]]
    require(sum(r["size"] for r in references[1:]) <= 1048576
            and references[0]["size"] <= 3072, "Preview exceeds file budgets")
    parts = [("manifest.json", root)]
    for ref in references:
        raw = read_blob(ref["sha256"])
        require(len(raw) == ref["size"] and digest(raw) == ref["sha256"],
                "Published preview object is damaged", ref["path"], code="SERVICE_UNAVAILABLE")
        parts.append((ref["path"], raw))
    # Deterministic framing; even an adversarial binary containing a candidate
    # delimiter must not create an ambiguous MIME representation.
    counter = 0
    while True:
        boundary = "ideas-" + digest(root + locale.encode() + str(counter).encode())
        if all(boundary.encode() not in raw for _, raw in parts):
            break
        counter += 1
    output = bytearray()
    for index, (path, raw) in enumerate(parts):
        media = "application/json" if index < 3 else "application/octet-stream"
        output.extend((f"--{boundary}\r\nContent-Type: {media}\r\n"
                       f"Content-Location: {path}\r\nContent-Length: {len(raw)}\r\n\r\n").encode())
        output.extend(raw)
        output.extend(b"\r\n")
    output.extend(f"--{boundary}--\r\n".encode())
    require(len(output) <= PREVIEW_BYTES, "Preview exceeds response budget")
    media = f'multipart/related; type="application/json"; boundary="{boundary}"'
    return bytes(output), media

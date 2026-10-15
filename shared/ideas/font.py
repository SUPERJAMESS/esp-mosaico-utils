"""Publication-side shaping and MIFF/1 serialization; no GSP dependency.

Requires freetype-py and the host's libraqm (FreeType/HarfBuzz/FriBidi).
Shaping is performed on the publication host, never on the device.
"""
import ctypes as C
from ctypes.util import find_library
import struct

import freetype


class Glyph(C.Structure):
    _fields_ = [("index", C.c_uint), ("x_advance", C.c_int),
                ("y_advance", C.c_int), ("x_offset", C.c_int),
                ("y_offset", C.c_int), ("cluster", C.c_uint32), ("face", C.c_void_p)]


class Font:
    def __init__(self, path, pixels, locale):
        library = find_library("raqm")
        if not library:
            raise RuntimeError("Install libraqm on the publication host (for example: Debian package libraqm0)")
        self.raqm = C.CDLL(library)
        signatures = {
            "raqm_create": (C.c_void_p, []),
            "raqm_destroy": (None, [C.c_void_p]),
            "raqm_set_text_utf8": (C.c_bool, [C.c_void_p, C.c_char_p, C.c_size_t]),
            "raqm_set_freetype_face": (C.c_bool, [C.c_void_p, C.c_void_p]),
            "raqm_set_freetype_load_flags": (C.c_bool, [C.c_void_p, C.c_int]),
            "raqm_set_language": (C.c_bool, [C.c_void_p, C.c_char_p, C.c_size_t, C.c_size_t]),
            "raqm_set_par_direction": (C.c_bool, [C.c_void_p, C.c_int]),
            "raqm_get_par_resolved_direction": (C.c_int, [C.c_void_p]),
            "raqm_layout": (C.c_bool, [C.c_void_p]),
            "raqm_get_glyphs": (C.POINTER(Glyph), [C.c_void_p, C.POINTER(C.c_size_t)]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.raqm, name)
            function.restype, function.argtypes = result, arguments
        self.face = freetype.Face(str(path))
        self.face.set_pixel_sizes(0, pixels)
        self.pixels, self.locale = pixels, locale
        self.bitmaps = {}
        self.line_height = (self.face.size.height + 63) // 64
        self.ascender = (self.face.size.ascender + 63) // 64

    def shape(self, text, direction=0):
        if not text:
            return [], direction, 0
        raw = text.encode("utf-8")
        rq = self.raqm.raqm_create()
        if not rq:
            raise MemoryError("Could not create text layout")
        try:
            if not (self.raqm.raqm_set_text_utf8(rq, raw, len(raw)) and
                    self.raqm.raqm_set_freetype_face(rq, self.face._FT_Face) and
                    self.raqm.raqm_set_freetype_load_flags(rq, freetype.FT_LOAD_DEFAULT) and
                    self.raqm.raqm_set_language(rq, self.locale.encode("ascii"), 0, len(raw)) and
                    self.raqm.raqm_set_par_direction(rq, direction) and self.raqm.raqm_layout(rq)):
                raise ValueError("Could not shape the published text")
            count = C.c_size_t()
            glyphs = self.raqm.raqm_get_glyphs(rq, C.byref(count))
            if not glyphs and count.value:
                raise ValueError("Text layout returned no glyphs")
            result, x = [], 0
            for i in range(count.value):
                g = glyphs[i]
                if not g.index:
                    raise ValueError("The font does not cover the published text")
                if g.y_advance:
                    raise ValueError("Only horizontal text is supported")
                result.append((g.index, x + g.x_offset, -g.y_offset, g.cluster))
                x += g.x_advance
            return result, self.raqm.raqm_get_par_resolved_direction(rq), x
        finally:
            self.raqm.raqm_destroy(rq)

    def bitmap(self, gid):
        if gid not in self.bitmaps:
            self.face.load_glyph(gid, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
            slot = self.face.glyph
            b = slot.bitmap
            if b.width and b.rows and b.pixel_mode != freetype.FT_PIXEL_MODE_GRAY:
                raise ValueError("Only monochrome A8 glyphs are supported; use a PNG for color artwork")
            rows = bytes(b.buffer)
            alpha = b"".join(rows[y * abs(b.pitch):y * abs(b.pitch) + b.width]
                             for y in range(b.rows))
            if b.pitch < 0:
                alpha = b"".join(alpha[y * b.width:(y + 1) * b.width] for y in reversed(range(b.rows)))
            self.bitmaps[gid] = (b.width, b.rows, slot.bitmap_left, slot.bitmap_top,
                                 slot.advance.x, alpha)
        return self.bitmaps[gid]

    def layout(self, text, width, height):
        runs, line_index = [], 0
        for paragraph in text.split("\n"):
            shaped, direction, _ = self.shape(paragraph)
            # Break only at shaping-cluster boundaries, preserving combining
            # sequences and ligatures. Prefer spaces when a line overflows.
            encoded = paragraph.encode("utf-8")
            boundaries = sorted({g[3] for g in shaped} | {0, len(encoded)})
            clusters = [encoded[a:b].decode("utf-8") for a, b in zip(boundaries, boundaries[1:])]
            lines, current = [], ""
            for cluster in clusters:
                if self.shape(current + cluster, direction)[2] > (width - 4) * 64:
                    split = current.rfind(" ")
                    if split > 0:
                        lines.append(current[:split])
                        current = current[split + 1:] + cluster
                    elif current:
                        lines.append(current)
                        current = cluster
                    else:
                        raise ValueError("A text cluster exceeds the available width")
                    if self.shape(current, direction)[2] > (width - 4) * 64:
                        raise ValueError("Text cannot be wrapped into the available width")
                else:
                    current += cluster
            lines.append(current)
            for line in lines:
                glyphs, _, advance = self.shape(line, direction)
                # Allow ink overhang (e.g. the negative left bearing of T),
                # while keeping RTL glyph order and right alignment intact.
                extents = [((x + 32) // 64 + self.bitmap(gid)[2],
                            (x + 32) // 64 + self.bitmap(gid)[2] + self.bitmap(gid)[0])
                           for gid, x, _, _ in glyphs if self.bitmap(gid)[0]]
                left = min([0] + [a for a, _ in extents])
                right = max([(advance + 63) // 64] + [b for _, b in extents])
                origin = width - 2 - right if direction == 1 else 2 - left
                for gid, x, y, _ in glyphs:
                    w, h, left, top, _, _ = self.bitmap(gid)
                    x = origin + (x + 32) // 64
                    y = line_index * self.line_height + self.ascender + (y + 32) // 64
                    if w and h and (x + left < 0 or x + left + w > width or
                                    y - top < 0 or y - top + h > height):
                        raise ValueError("Text does not fit the display; shorten it before publishing")
                    runs.append([gid, x, y])
                line_index += 1
        if text and not runs:
            raise ValueError("Nonempty text must contain glyphs; use an empty description for no text")
        if len(runs) > 512:
            raise ValueError("Text exceeds the bounded glyph-run limit")
        return dict(text=text, width=width, height=height, glyphs=runs)

    def encode(self):
        # Empty descriptions still have a valid font resource.
        if not self.bitmaps:
            self.bitmap(self.face.get_char_index(ord(" ")))
        count = len(self.bitmaps)
        if not 0 < count <= 512:
            raise ValueError("Font subset exceeds the glyph limit")
        offset, table, data = 24 + 20 * count, bytearray(), bytearray()
        for gid, (w, h, left, top, advance, alpha) in sorted(self.bitmaps.items()):
            if not gid or w > 192 or h > 192 or abs(left) > 192 or abs(top) > 192 or abs(advance) > 192 * 64:
                raise ValueError("Font metrics exceed the device limits")
            table += struct.pack("<IIHHhhi", gid, offset, w, h, left, top, advance)
            data += alpha
            offset += len(alpha)
        if offset > 1024 * 1024 or not 0 < self.line_height <= 192:
            raise ValueError("Font subset exceeds the device limits")
        return struct.pack("<4sHHIHHHHI", b"MIFF", 1, 24, offset, count,
                           self.pixels, self.line_height, 0, 24) + table + data

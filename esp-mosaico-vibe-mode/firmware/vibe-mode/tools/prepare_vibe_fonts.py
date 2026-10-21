#!/usr/bin/env python3
"""Keep the reviewed Latin typography with a subset Chinese fallback.

Only used when authoring translations. Normal builds consume checked-in fonts.
Latin outlines and advances come from the reference Inter/Archivo fonts; CJK
outlines come from Noto Sans CJK SC. All sources retain their OFL licenses.
"""
import json
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.qu2cuPen import Qu2CuPen
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont

UI = Path(__file__).resolve().parents[1] / "ui"


def build(cjk_source):
    cjk = TTFont(cjk_source, fontNumber=2 if str(cjk_source).endswith('.ttc') else -1)
    translations = json.loads((UI / 'translations.json').read_text(encoding="utf-8"))
    chars = set(''.join(translations.keys()) + ''.join(translations.values()))
    chars.update(chr(i) for i in range(32, 127))
    chars.update('‹›•↑↓°·×–—…简体中文')
    # The generated scene also includes backend literals that are not entries
    # in translations.json (for example an already-localized failure detail).
    scene = json.loads((UI / 'main.json').read_text(encoding="utf-8"))
    for obj in scene['objects']:
        chars.update(obj.get('text', '') + obj.get('font_charset', ''))
    for source, name in [('Inter-Regular.ttf', 'VibeInter'),
                         ('ArchivoExtended-Medium.ttf', 'VibeMedium'),
                         ('ArchivoExtended-SemiBold.ttf', 'VibeDisplay')]:
        latin = TTFont(UI / 'fonts' / source)
        weight = {'VibeInter': 'Regular', 'VibeMedium': 'Medium', 'VibeDisplay': 'Bold'}[name]
        reference_cjk = TTFont(UI / 'fonts' / ('NotoSansSC-' + weight + '.otf'))
        glyphs, metrics, cmap = {}, {}, {}
        order = ['.notdef']
        pen = T2CharStringPen(500, None)
        glyphs['.notdef'] = pen.getCharString()
        metrics['.notdef'] = (500, 0)
        for char in sorted(chars):
            code = ord(char)
            font = latin if code in latin.getBestCmap() else reference_cjk if code in reference_cjk.getBestCmap() else cjk
            original = font.getBestCmap().get(code)
            if original is None:
                if char in '\r\n\t':
                    continue
                raise ValueError(f'{name}: missing glyph {char!r} (U+{code:04X})')
            glyph = 'uni%04X' % code
            scale = 1000 / font['head'].unitsPerEm
            width, bearing = font['hmtx'][original]
            width, bearing = round(width * scale), round(bearing * scale)
            pen = T2CharStringPen(width, None)
            target = TransformPen(Qu2CuPen(pen, max_err=0.5, all_cubic=True), (scale, 0, 0, scale, 0, 0))
            recording = DecomposingRecordingPen(font.getGlyphSet())
            font.getGlyphSet()[original].draw(recording)
            recording.replay(target)
            glyphs[glyph] = pen.getCharString()
            metrics[glyph] = (width, bearing)
            cmap[code] = glyph
            order.append(glyph)
        fb = FontBuilder(1000, isTTF=False)
        fb.setupGlyphOrder(order)
        fb.setupCharacterMap(cmap)
        fb.setupCFF(name, {'FullName': name, 'FamilyName': name, 'Weight': 'Regular'}, glyphs, {})
        fb.setupHorizontalMetrics(metrics)
        # Match the reference font's line box so its labels stay on the same baseline.
        scale = 1000 / latin['head'].unitsPerEm
        asc, desc = round(latin['hhea'].ascent * scale), round(latin['hhea'].descent * scale)
        fb.setupHorizontalHeader(ascent=asc, descent=desc)
        fb.setupNameTable({'familyName': name, 'styleName': 'Regular', 'uniqueFontIdentifier': name,
                          'fullName': name, 'psName': name, 'version': 'Version 1.0'})
        fb.setupOS2(sTypoAscender=asc, sTypoDescender=desc, usWinAscent=max(asc, 1160), usWinDescent=max(-desc, 300))
        fb.setupPost()
        fb.save(UI / 'fonts' / (name + '.otf'))


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cjk_source', type=Path)
    build(parser.parse_args().cjk_source)

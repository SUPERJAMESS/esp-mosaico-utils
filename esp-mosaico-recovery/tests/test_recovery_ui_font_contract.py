"""The fixed-slot font subset must cover every runtime update title."""
import json
from pathlib import Path
import re

RECOVERY = Path(__file__).resolve().parents[1] / 'firmware/recovery'


def test_compiled_update_font_covers_device_and_simulator_states():
    scene = json.loads((RECOVERY / 'ui/main.json').read_text())
    objects = {item.get('name'): item for item in scene['objects']}
    titles = []
    for source in ('components/app_ui/src/ui_services.c', 'pc/platform_pc.c'):
        statements = re.findall(
            r'(?:COPY\s*\(\s*update_title|snprintf\s*\(\s*model\.update_title)[^;]+;',
            (RECOVERY / source).read_text(), re.S)
        for statement in statements:
            titles.extend(text for text in re.findall(r'"([^"\\]*)"', statement)
                          if text != '%s')
    assert titles, 'update title producers were not found'
    for name in ('update_title', 'result_title'):
        obj = objects[name]
        glyphs = set(obj['font_charset'] + obj.get('text', ''))
        for title in titles:
            assert set(title) <= glyphs, (name, title, set(title) - glyphs)
    assert set('0123456789%') <= set(objects['update_percent']['font_charset'])
    assert '-' in objects['update_percent']['font_charset']
    assert set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-') <= set(objects['bridge_code_compact']['font_charset'])

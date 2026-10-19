"""The fixed-slot font subset must cover every runtime update title."""
import json
from pathlib import Path
import re
import struct
import zlib

RECOVERY = Path(__file__).resolve().parents[1] / 'firmware/vibe-mode'


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


def test_all_compiled_glyphs_have_pixels_and_dynamic_translation_coverage():
    """Audit the actual embedded atlas, including generated keyboard glyphs."""
    packed = (RECOVERY / 'components/app_ui/light/vui_assets.bin').read_bytes()
    data = zlib.decompress(packed[4:])
    _, size, nodes, count, fonts, font_count, _, _ = struct.unpack_from('<8I', data)
    assert size == len(data)
    atlases = []
    for i in range(font_count):
        offset, total = struct.unpack_from('<II', data, fonts + i * 16)
        chars = set()
        for n in range(total):
            cp, pixels, _, _, w, h, _, _ = struct.unpack_from('<II6h', data, offset + n * 20)
            chars.add(chr(cp))
            if not chr(cp).isspace():
                assert w > 0 and h > 0 and any(data[pixels:pixels + w * h]), (i, chr(cp))
        atlases.append(chars)
    scene = json.loads((RECOVERY / 'ui/main.json').read_text())
    translations = json.loads((RECOVERY / 'ui/translations.json').read_text())
    for i, obj in enumerate(scene['objects']):
        if 'text' not in obj:
            continue
        node = struct.unpack_from('<5h15HI', data, nodes + i * 44)
        chars = atlases[node[12]]
        # Literal and dynamic source charsets must survive into the device pack.
        required = set(obj.get('font_charset', '') + obj['text']) - set('\r\n')
        assert required <= chars, (obj.get('name'), required - chars)
        dynamic = {
            'shared_value': ('Enter text',),
            'update_rate': ('Receive', 'Download', 'Read', 'Write'),
            'delete_confirm_title': ('Delete Idea', 'Shared information'),
            'delete_note': ('Remove this saved Idea?\nYou can undo for 4 seconds.',
                            'You can undo for 4 seconds.'),
            'update_title': ('Checking image', 'Saving Idea', 'Verifying files'),
            'result_title': ('Checking image', 'Saving Idea', 'Verifying files'),
            'update_detail': ('Receiving application', 'Receiving system component',
                              'Checking installed application', 'Preparing application'),
        }
        if obj.get('bind') in dynamic:
            required = set(''.join(translations[s] for s in dynamic[obj['bind']])) - set('\r\n')
            assert required <= chars, (obj['bind'], required - chars)


def test_online_firmware_status_fonts_cover_translations():
    scene = json.loads((RECOVERY / 'ui/main.json').read_text())
    objects = {item.get('name'): item for item in scene['objects']}
    translations = json.loads((RECOVERY / 'ui/translations.json').read_text())
    for name, strings in {
        'firmware_progress_title': ['Updating firmware', 'Firmware update failed'],
        'firmware_phase': ['Downloading firmware', 'Verifying firmware', 'Writing firmware', 'Restarting Vibe Mode'],
        'firmware_power_warning': ['Do not unplug or power off.'],
    }.items():
        glyphs = set(objects[name]['font_charset'])
        for text in strings:
            assert set(text + translations[text]) <= glyphs, (name, text)

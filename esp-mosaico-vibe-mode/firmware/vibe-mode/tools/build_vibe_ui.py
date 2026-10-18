#!/usr/bin/env python3
"""Generate the product navigation around the retained, tested core scenes.

core.json owns Wi-Fi/keyboard/preview/transfer widgets. This file owns product
pages. main.json and vibe_locale.inc are generated; no runtime demo data.
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "ui"
COLORS = {"bg": "#000000", "card": "#181818", "text": "#ECECE8", "muted": "#999999", "orange": "#FF4C01", "red": "#E5484D"}


def generate():
    scene = json.loads((UI / "core.json").read_text())
    objects = scene["objects"]
    named = {o.get("name"): o for o in objects if o.get("name")}

    def add(kind, parent, x, y, w, h, **props):
        objects.append(dict(type=kind, parent=parent, x=x, y=y, w=w, h=h, **props))
        return len(objects) - 1

    def label(parent, name, text, x=24, y=80, w=432, h=40, size=20, color=None, align="left"):
        return add("label", parent, x, y, w, h, name=name, bind=name, text=text,
                   font_size=size, fg_color=color or COLORS["text"], text_align=align)

    def button(parent, name, text, x, y, w, h=64, primary=False, danger=False, action=None, arg=None):
        event = {"event": "click", "action": "call", "target_name": action or name}
        if arg is not None:
            event["arg"] = arg
        return add("button", parent, x, y, w, h, name="button_" + name, bind=name + "_label", text=text,
                   font_size=20, fg_color=COLORS["text"], bg_color=COLORS["red"] if danger else COLORS["orange"] if primary else COLORS["card"],
                   radius=18, events=[event])

    def page(name, title):
        p = add("layer", -1, 0, 0, 480, 480, name=name, bind=name + "_visible", bind_target="visible", hidden=True)
        button(p, "back_" + name, "‹", 16, 16, 56, 56, action="product_back")
        label(p, name + "_title", title, 88, 20, 368, 48, 24)
        return p

    # Keep the core bindings available while replacing the home composition.
    for obj in objects[1:15]:
        obj["hidden"] = True
    label(0, "home_brand", "Vibe Mode", 24, 18, 260, 36)
    label(0, "home_status", "", 260, 18, 196, 36, 16, align="right")
    label(0, "home_mark", "{ * }", 24, 74, 432, 88, 32, COLORS["orange"], "center")
    label(0, "home_headline", "Bring your Ideas to life", 24, 170, 432, 56, 24, align="center")
    label(0, "home_caption", "Make one. Download one. Run it here.", 24, 232, 432, 44, 16, COLORS["muted"], "center")
    for i, (name, title) in enumerate((("develop", "Develop"), ("manage", "Manage"), ("settings", "Settings"))):
        button(0, "nav_" + name, title, 16 + i * 152, 304, 144, 112)
    label(0, "home_hint", "Swipe left for your Ideas  ›", 24, 432, 432, 30, 16, COLORS["muted"], "center")

    # Idea detail: opening replaces the old separate install/restart actions.
    named["button_install"].update(text="Open", y=337, h=56)
    named["button_start_idea"].update(text="Delete", bg_color=COLORS["red"], events=[{"event": "click", "action": "call", "target_name": "delete_idea"}])
    named["button_update_idea"].update(text="Download latest")
    named["button_nand_scan"].update(x=252, w=204, text="Update all", bind="batch_button", events=[{"event": "click", "action": "call", "target_name": "update_all"}])
    for obj in objects:
        if obj.get("parent") == 59 and obj.get("text") == "My Ideas": obj["text"] = "Manage Ideas"
    button(59, "manage_add", "+", 186, 79, 56, 44, action="download")
    # Live rows use a visibility gate to invalidate the retained viewport after
    # rebinding; each retained row has independent text and image state.
    named["bundles"].update(h=252, scroll_snapshot=False, bind="bundles_visible", bind_target="visible")
    named["nand_scan"].update(w=154)
    label(59, "undo_message", "", 24, 408, 300, 52, 16)
    button(59, "undo_delete", "Undo", 344, 408, 112, 52)
    button(80, "cancel_download", "Cancel", 152, 408, 176, 52)
    button(91, "result_library", "My Ideas", 24, 310, 204, 56, action="result_done")
    button(91, "result_open", "Open", 252, 310, 204, 56)

    develop = page("develop", "Develop")
    label(develop, "develop_usb", "USB connection", y=96)

    settings = page("settings", "Settings")
    for i, (name, title) in enumerate((("wifi", "Wi-Fi"), ("display", "Display & haptics"), ("language", "Language"),
                                       ("shared", "Shared information"), ("about", "About & help"), ("reset", "Reset settings"))):
        button(settings, "nav_" + name, title, 24 + (i % 2) * 220, 100 + (i // 2) * 112, 212, 96, danger=name == "reset")

    display = page("display", "Display & haptics")
    label(display, "brightness_title", "Brightness", y=88)
    for i in range(5): button(display, "brightness_" + str(i), str((i + 1) * 20) + "%", 24 + i * 88, 132, 80, 56, action="brightness", arg=(i + 1) * 20)
    label(display, "timeout_title", "Screen off", y=204)
    for i, (title, seconds) in enumerate((("1 min", 60), ("3 min", 180), ("5 min", 300), ("10 min", 600), ("Never", 0))):
        button(display, "timeout_" + str(i), title, 24 + i * 88, 248, 80, 56, action="timeout", arg=seconds)
    button(display, "toggle_rotate", "Auto rotate: on", 24, 332, 212, 64)
    button(display, "toggle_haptics", "Haptics: on", 244, 332, 212, 64)
    label(display, "display_summary", "", y=408, h=48, size=16)

    for name, title in (("language", "Language"), ("welcome", "Welcome to Vibe Mode")):
        p = page(name, title)
        label(p, name + "_note", "Choose your language", y=120, align="center")
        button(p, name + "_en", "English", 48, 200, 384, 80, action="set_language", arg=1)
        button(p, name + "_zh", "简体中文", 48, 304, 384, 80, action="set_language", arg=2)
        if name == "welcome": objects[p + 1]["hidden"] = True

    shared = page("shared", "Shared information")
    for i in range(4): button(shared, "shared_row_" + str(i), "", 24, 90 + i * 72, 432, 64, action="shared_row", arg=i)
    button(shared, "shared_prev", "‹", 24, 394, 80, 64)
    button(shared, "shared_add", "+", 116, 394, 248, 64, primary=True)
    button(shared, "shared_next", "›", 376, 394, 80, 64)
    editor = page("shared_editor", "Shared information")
    label(editor, "shared_field", "Key", y=84)
    label(editor, "shared_value", "", y=126, h=68, size=20)
    button(editor, "shared_reveal", "Show", 24, 196, 128, 40)
    button(editor, "shared_delete", "Delete", 164, 196, 128, 40, danger=True)
    button(editor, "shared_submit", "Next", 304, 196, 152, 40, primary=True)
    kb = dict(named["password_keyboard"])
    kb.update(parent=editor, name="shared_keyboard", y=242, h=222)
    objects.append(kb)

    about = page("about", "About & help")
    label(about, "about_version", "", y=96, h=48)
    label(about, "about_storage", "", y=152, h=48)
    label(about, "about_mac", "", y=208, h=40, size=16)
    label(about, "about_help", "Hold AI and double-press Power\nto return to Vibe Mode.\nHold Boot during reset for ROM Download Mode.", y=266, h=96, size=16)
    button(about, "about_pairing", "TCP pairing", 24, 388, 212, 64, action="pairing")
    button(about, "about_download", "Find Ideas", 244, 388, 212, 64, action="download")

    reset = page("reset", "Reset settings")
    label(reset, "reset_note", "Clear Wi-Fi, shared information\nand system preferences.\nNAND and all saved Ideas are kept.", y=112, h=132)
    button(reset, "reset_hold", "Reset settings", 24, 348, 432, 96, danger=True, action="none")

    grid = page("grid", "My Ideas")
    for i in range(6):
        x, y = 24 + (i % 3) * 148, 92 + (i // 3) * 168
        add("container", grid, x - 3, y - 3, 142, 150, name="cell_current_" + str(i), bg_color=COLORS["orange"], radius=20)
        button(grid, "cell_" + str(i), "", x, y, 136, 144, action="open_cell", arg=i)
        add("image", grid, x + 26, y + 8, 84, 84, name="cell_default_" + str(i), image="assets/idea-default.svg")
        add("image", grid, x + 26, y + 8, 84, 84, name="cell_icon_" + str(i), bind="cell_icon_" + str(i), image="assets/idea-default.svg", dynamic_image=True, bind_target="resource")
        label(grid, "cell_name_" + str(i), "", x + 4, y + 96, 128, 40, 16, align="center")
    label(grid, "grid_page", "", y=432, h=28, size=16, align="center")

    confirm = page("open_confirm", "Open Idea")
    label(confirm, "open_name", "", y=100, h=40, size=20)
    label(confirm, "open_question", "Leave Vibe Mode and open this Idea?", y=150, h=64, size=20)
    label(confirm, "open_warning", "", y=220, h=68, color=COLORS["orange"])
    button(confirm, "toggle_open_confirm", "Always ask before opening", 24, 294, 432, 42)
    button(confirm, "open_cancel", "Cancel", 24, 352, 204, 88, action="product_back")
    button(confirm, "open_accept", "Open", 252, 352, 204, 88, primary=True)

    delete = page("delete_confirm", "Delete Idea")
    label(delete, "delete_note", "Remove this saved Idea?\nYou can undo for 4 seconds.", y=132, h=100)
    label(delete, "delete_progress", "Hold for 3 seconds to delete", y=276, align="center")
    button(delete, "delete_hold", "Delete", 24, 348, 432, 96, danger=True, action="none")

    sleep = page("sleep", "Vibe Mode")
    label(sleep, "sleep_message", "Touch or pick up to wake", y=200, h=80, size=24, align="center")
    objects[sleep + 1]["hidden"] = True
    boot = page("boot_tip", "ROM Download Mode")
    label(boot, "boot_note", "Hold Boot and double-press Power.\nThe chip will enter ROM Download Mode.\nVibe Mode does not run in that mode.", y=120, h=160)

    from vibe_design import FONT, apply
    apply(scene)

    # Conditional controls share visibility gates between device and simulator.
    gates = ["button_toggle_open_confirm", "button_cancel_download", "button_result_open",
             "button_undo_delete", "button_local_delete", "button_start_idea", "button_check_idea", "button_update_idea", "button_accept_idea", "button_shared_reveal", "button_shared_delete"]
    gates += ["cell_icon_" + str(i) for i in range(6)]
    gates += ["button_cell_" + str(i) for i in range(6)]
    for name in gates:
        obj = next(o for o in objects if o.get("name") == name)
        obj["parent"] = add("layer", obj["parent"], 0, 0, 480, 480,
                            name=name + "_gate", bind=name + "_visible", bind_target="visible")
    for obj in objects:
        if obj.get("name", "").startswith(("cell_current_", "cell_default_")):
            obj.update(bind=obj["name"] + "_visible", bind_target="visible")

    firmware = page("firmware_notice", "Update Vibe Mode")
    label(firmware, "firmware_available", "Update available", y=100, size=24, align="center")
    label(firmware, "firmware_version", "", y=152, size=24, align="center")
    label(firmware, "firmware_warning", "Do not unplug or power off.", y=212, h=40, color=COLORS["red"], align="center")
    label(firmware, "firmware_ready", "", y=270, h=48, size=16, align="center")
    button(firmware, "firmware_later", "Skip", 24, 360, 204, action="home")
    button(firmware, "firmware_start", "Start update", 252, 360, 204, primary=True)

    installing = add("layer", -1, 0, 0, 480, 480, name="firmware_progress", bind="firmware_progress_visible", bind_target="visible", hidden=True)
    label(installing, "firmware_progress_title", "Updating firmware", y=36, size=24, align="center")
    label(installing, "firmware_phase", "", y=132, align="center")
    percent = label(installing, "firmware_percent", "", y=196, size=32, align="center")
    objects[percent]["font"] = FONT["doto"]
    label(installing, "firmware_power_warning", "Do not unplug or power off.", y=268, color=COLORS["red"], align="center")
    failed = add("layer", installing, 0, 0, 480, 480, name="firmware_failed", bind="firmware_failed_visible", bind_target="visible", hidden=True)
    button(failed, "firmware_failure_back", "Back", 96, 380, 288, action="home")

    translations = json.loads((UI / "translations.json").read_text())
    from vibe_design import runtime_charsets
    dynamic_chars = runtime_charsets(ROOT, translations)
    chars = set("".join(translations.values()))
    latin = "".join(chr(i) for i in range(32, 127)) + "‹›•↑简体中文"
    charset = latin + "".join(sorted(chars - set(latin)))
    scene["font"] = "fonts/VibeInter.otf"
    scene["font_size"] = 16
    # Each size has a single shared atlas. Runtime English remains bounded ASCII;
    # runtime Idea presentation keeps the existing externally shaped glyph path.
    static_labels = []
    for i, obj in enumerate(objects):
        if "text" in obj:
            # Compile only the characters each style can display. Keep the
            # reference's size and font rather than flattening all text to 20px.
            value = obj.get("text", "")
            obj["font_charset"] = latin + value + translations.get(value, "")
            obj["font_charset"] += dynamic_chars.get(obj.get("bind"), "")
            if obj.get("name", "").startswith("code_digit_") or obj.get("name") in ("update_percent", "firmware_percent"):
                obj["font_charset"] = "0123456789% -"
            if obj.get("name") == "home_ideas_count":
                obj["font_charset"] = "0123456789-"
            if obj.get("font") == "fonts/NotoSansSC-Bold.otf":
                obj["font_charset"] = value
            if obj.get("font") == "fonts/ArchivoExtended-Black.ttf" or obj.get("name") == "home_headline":
                obj["font_charset"] = value
            if obj.get("hidden"):
                obj["font_size"] = 16
                obj["font"] = "fonts/VibeInter.otf"
                obj["font_charset"] = latin
            if "\n" in value:
                obj["text_align"] = "left"
            parent = obj.get("parent", -1)
            in_list = False
            while parent >= 0:
                in_list |= objects[parent]["type"] == "list" or "template" in objects[parent]
                parent = objects[parent].get("parent", -1)
            if in_list or obj.get("hidden"):
                continue
            if not obj.get("bind"):
                obj["bind"] = "static_" + str(i)
            if obj.get("text"):
                static_labels.append((obj["bind"], obj["text"]))

        if obj["type"] == "keyboard": obj["font"] = "fonts/VibeMedium.otf"
    (UI / "main.json").write_text(json.dumps(scene, indent=2, ensure_ascii=False) + "\n")
    def c(value): return json.dumps(value, ensure_ascii=False)
    lines = ["// Generated by tools/build_vibe_ui.py; do not edit.",
             "static const struct { const char *en, *zh; } vibe_translations[] = {"]
    lines += [f"    {{{c(en)}, {c(zh)}}}," for en, zh in translations.items()]
    lines += ["};", "static const struct { uint16_t bind; const char *text; } vibe_static_labels[] = {"]
    lines += [f"    {{VUI_VIBE_BIND_{name.upper()}, {c(value)}}}," for name, value in static_labels]
    lines += ["};\n"]
    (ROOT / "components/app_ui/private/vibe_locale.inc").write_text("\n".join(lines))


def prepare_font(source):
    """Optional authoring step; normal firmware builds use the checked-in subset."""
    from prepare_vibe_fonts import build
    build(source)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font-source", type=Path, help="Rebuild subset from Noto Sans CJK SC OTF or collection face 2 (requires fonttools)")
    args = parser.parse_args()
    generate()
    if args.font_source:
        prepare_font(args.font_source)

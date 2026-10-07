#!/usr/bin/env python3
"""Generate ui/main.json.

The scene is four full-screen pages stacked on the root, each a container whose
visibility is bound so C can show one at a time. Writing it by hand meant
hand-maintaining a few hundred coordinates across four pages, and the settings
rows are the same shape repeated six times, so they are emitted from one
function instead of copied.

Run from this directory:  python build_scene.py
"""

import json

W = H = 480

BG        = "#07090C"
PANEL     = "#0E1217"
RAISED    = "#1E222A"
LINE      = "#232A33"
TEXT      = "#E8EAED"
DIM       = "#8B939E"
ACCENT    = "#FF4C01"
SCRIM_TOP = "#0A0D12C4"
SCRIM_BOT = "#0A0D12F2"

objects = []


def add(**o):
    objects.append(o)
    return len(objects) - 1


def page(name, visible):
    """A full-screen page. Only one is visible at a time, driven from C."""
    return add(type="container", parent=-1, name=name, bind=name,
               bind_target="visible", x=0, y=0, w=W, h=H,
               bg_color=BG if not visible else "#00000000",
               hidden=not visible, radius=0)


def header(parent, title, back_cb):
    add(type="container", parent=parent, x=0, y=0, w=W, h=64,
        bg_color=PANEL, radius=0)
    add(type="button", parent=parent, name="btn_" + back_cb, callback=back_cb,
        x=8, y=8, w=48, h=48, radius=24, bg_color="#00000000")
    add(type="image", parent=parent, image="icons/back.png",
        x=18, y=18, w=28, h=28)
    add(type="label", parent=parent, x=64, y=20, w=300, h=26,
        text=title, font_size=20, fg_color=TEXT,
        font="fonts/DejaVuSans-Bold.ttf")


def row(parent, index, name, label, callback, value_bind=None, value="",
        chevron=True):
    """One settings line: label on the left, live value and chevron right.

    chevron=False marks a row that shows a value but does not change it on
    tap, so the arrow does not promise an action the row will not perform.
    """
    y = 72 + index * 58
    add(type="button", parent=parent, name="btn_" + name, callback=callback,
        x=12, y=y, w=456, h=52, radius=12, bg_color=RAISED)
    add(type="label", parent=parent, x=28, y=y + 15, w=200, h=24,
        text=label, font_size=17, fg_color=TEXT)
    v = dict(type="label", parent=parent, x=236, y=y + 16, w=192, h=22,
             text=value, font_size=15, fg_color=DIM, text_align="right")
    if value_bind:
        v["name"] = value_bind
        v["bind"] = value_bind
    add(**v)
    if chevron:
        add(type="label", parent=parent, x=440, y=y + 14, w=14, h=24,
            text=">", font_size=17, fg_color=DIM)


# ---------------------------------------------------------------- page: scan
scan = page("page_scan", visible=True)

add(type="image", parent=scan, name="preview", bind="preview",
    bind_target="resource", image="placeholder.png", fit="cover",
    x=0, y=0, w=W, h=H)

# Tapping the frame shoots, the same as the shutter. Sits under the bars.
add(type="button", parent=scan, name="frame", callback="shoot",
    x=0, y=56, w=W, h=232, radius=0, bg_color="#00000000")

add(type="container", parent=scan, x=0, y=0, w=W, h=56,
    bg_color=SCRIM_TOP, radius=0)
add(type="image", parent=scan, image="icons/wifi.png", x=18, y=16, w=24, h=24)
add(type="label", parent=scan, name="net_name", bind="net_name",
    text="Not set up", x=52, y=9, w=250, h=22, font_size=17, fg_color=TEXT)
add(type="label", parent=scan, name="net_state", bind="net_state",
    text="tap the gear to begin", x=52, y=30, w=250, h=18,
    font_size=13, fg_color=DIM)
add(type="label", parent=scan, name="model_chip", bind="model_chip",
    text="", x=312, y=19, w=152, h=20, font_size=13, fg_color=DIM,
    text_align="right")

# The answer panel.
#
# One label held both the part number and the description, and the two ran
# together into a block of orange that had to be read twice to find the part
# number in it. They are different things, so they get different rows: the part
# number big and in the accent, what it is underneath in plain text, a hairline
# between them. Nothing merges when the rows cannot share a line.
#
# The whole panel moved up 24 px to pay for the second row, because the row of
# buttons below it is fixed and the description needs two lines of its own.
add(type="container", parent=scan, x=0, y=288, w=W, h=192,
    bg_color=SCRIM_BOT, radius=0)
add(type="label", parent=scan, name="status", bind="status",
    text="Point at a component", x=24, y=294, w=432, h=18,
    font_size=14, fg_color=DIM)
# One line, ellipsis rather than wrap. A part number that does not fit is still
# recognisable from its start; a part number broken across two rows is not.
add(type="label", parent=scan, name="result", bind="result",
    text="Tap to identify", x=24, y=314, w=432, h=34, font_size=26,
    font="fonts/DejaVuSans-Bold.ttf", fg_color=ACCENT,
    text_align="left", overflow="ellipsis")
add(type="container", parent=scan, x=24, y=349, w=432, h=1,
    bg_color=LINE, radius=0)
# Two lines at font 16, which is the six words the prompt asks for and about
# 36 characters a line. text_align and overflow are spelled out because runtime
# bound multiline text needs them set explicitly. DejaVuSans at 16 measures 19
# px a line, so 38 would be the exact minimum and the second row of descenders
# would sit on the boundary; 40 buys the slack and still clears the buttons.
add(type="label", parent=scan, name="result_desc", bind="result_desc",
    text="", x=24, y=354, w=432, h=40, font_size=16, fg_color=TEXT,
    text_align="left", overflow="clip")

add(type="button", parent=scan, name="btn_flash", callback="flash",
    bind="flash_bg", bind_target="color",
    x=24, y=404, w=56, h=56, radius=28, bg_color=RAISED)
add(type="image", parent=scan, image="icons/flash.png",
    x=38, y=418, w=28, h=28)

add(type="button", parent=scan, name="btn_flip", callback="flip",
    x=92, y=404, w=56, h=56, radius=28, bg_color=RAISED)
add(type="image", parent=scan, image="icons/flip.png",
    x=106, y=418, w=28, h=28)

add(type="button", parent=scan, name="btn_shoot", callback="shoot",
    x=202, y=394, w=76, h=76, radius=38, bg_color="#00000000")
add(type="image", parent=scan, image="icons/shutter.png",
    x=202, y=394, w=76, h=76)

add(type="button", parent=scan, name="btn_settings", callback="settings",
    x=400, y=404, w=56, h=56, radius=28, bg_color=RAISED)
add(type="image", parent=scan, image="icons/gear.png",
    x=415, y=419, w=26, h=26)

# ------------------------------------------------------------ page: settings
st = page("page_settings", visible=False)
header(st, "Settings", "close_settings")
row(st, 0, "wifi",   "Wi-Fi",         "open_wifi",   "val_wifi",   "not set")
row(st, 1, "key",    "API key",       "open_key",    "val_key",    "not set")
row(st, 2, "model",  "Model",         "next_model",  "val_model",  "")
# Read only on the board. Cycling it here walked the value off 90 and every
# identification came back garbled, which looks like a bad model rather than a
# bad setting. It stays changeable from the portal, where the preview is
# visible on the board while the value is changed.
row(st, 3, "rot",    "Rotation",      "next_rot",    "val_rot",    "90",
    chevron=False)
row(st, 4, "mirror", "Mirror",        "flip",        "val_mirror", "off")
row(st, 5, "portal", "Setup portal",  "portal",      "val_portal", "off")
# Shown only while the portal is up: what to join and where to go.
add(type="label", parent=st, name="portal_info", bind="portal_info", text="",
    x=28, y=424, w=424, h=44, font_size=14, fg_color=ACCENT)

# ---------------------------------------------------------------- page: wifi
wf = page("page_wifi", visible=False)
header(wf, "Wi-Fi", "close_wifi")
add(type="label", parent=wf, name="wifi_hint", bind="wifi_hint",
    text="Scanning...", x=24, y=74, w=432, h=20, font_size=14, fg_color=DIM)
# Slots are fixed at compile time; a list cannot grow at runtime. Eight rows
# is more networks than anyone picks between, and C rewrites the text of the
# ones a scan actually found and blanks the rest.
WIFI_SLOTS = 8
add(type="list", parent=wf, name="wifi_list", bind="wifi_list",
    bind_target="data", x=12, y=100, w=456, h=320, item_height=52,
    bg_color=PANEL, fg_color=TEXT, radius=12,
    items=[" "] * WIFI_SLOTS, callback="pick_wifi")
add(type="button", parent=wf, name="btn_rescan", callback="rescan",
    x=12, y=428, w=456, h=44, radius=12, bg_color=RAISED)
add(type="label", parent=wf, x=12, y=441, w=456, h=22, text="Rescan",
    font_size=17, fg_color=TEXT, text_align="center")

# ------------------------------------------------- page: text entry/keyboard
tx = page("page_text", visible=False)
header(tx, "Enter", "close_text")
add(type="label", parent=tx, name="text_title", bind="text_title",
    text="", x=24, y=70, w=432, h=20, font_size=14, fg_color=DIM)
add(type="container", parent=tx, x=16, y=94, w=448, h=46,
    bg_color=RAISED, radius=10)
add(type="label", parent=tx, name="text_value", bind="text_value", input=True,
    text="", x=30, y=106, w=420, h=26, font_size=18, fg_color=TEXT)
# The keyboard takes no callback and no styling beyond a background: it is a
# fixed compile-time control group. C attaches it to the field's text bind with
# esp_gsp_keyboard_attach(), and that attachment is what delivers keys and OK.
add(type="keyboard", parent=tx, name="kb",
    x=0, y=150, w=W, h=330, bg_color=PANEL)

scene = {
    "screen": "partid",
    "w": W,
    "h": H,
    "screen_bg": BG,
    "font": "fonts/DejaVuSans.ttf",
    "default_font_size": 18,
    "swipe": False,
    # Runtime text can be any of these, so every glyph has to be baked in.
    "font_charset": (" !\"#$%&'()*+,-./0123456789:;<=>?@"
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
                     "abcdefghijklmnopqrstuvwxyz{|}~"),
    "objects": objects,
}

with open("main.json", "w", encoding="utf-8") as f:
    json.dump(scene, f, indent=2)
    f.write("\n")

print("wrote main.json with %d objects" % len(objects))

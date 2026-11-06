#!/usr/bin/env python3
"""Codec: green phosphor boxes with notched corners and brackets, on a grid with scanlines, all drawn
here (no artwork taken from anywhere).

    python3 make_codec.py          # writes Codec/: theme.json and the 9-slice PNGs

Copy Codec/ to the SD card as /mnt/themes/Codec/; the image does not install it. Format:
docs/THEMES.md. Needs Pillow.
"""
import json
import os

from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Codec")


def rgba(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def notch_mask(w, h, n):
    """Inside of a rectangle with its corners cut at 45 degrees, n pixels"""
    def inside(x, y):
        dx = min(x, w - 1 - x)
        dy = min(y, h - 1 - y)
        return dx + dy >= n - 1 or n <= 0
    return inside


def fill_notch(im, n, colour_at):
    w, h = im.size
    inside = notch_mask(w, h, n)
    px = im.load()
    for y in range(h):
        for x in range(w):
            if inside(x, y):
                px[x, y] = colour_at(x, y)


def stroke_notch(im, n, inset, col):
    """One pixel outline of the notched rectangle, inset pixels in"""
    w, h = im.size
    px = im.load()
    x0, y0, x1, y1 = inset, inset, w - 1 - inset, h - 1 - inset
    k = max(1, n - inset)
    for x in range(x0 + k, x1 - k + 1):
        px[x, y0] = col
        px[x, y1] = col
    for y in range(y0 + k, y1 - k + 1):
        px[x0, y] = col
        px[x1, y] = col
    for i in range(k + 1):       # the cut corners
        for (cx, cy) in ((x0 + i, y0 + k - i), (x1 - i, y0 + k - i), (x0 + i, y1 - k + i), (x1 - i, y1 - k + i)):
            px[cx, cy] = col


def brackets(im, length, inset, col, which="all"):
    """Corner brackets: an L with a small diagonal, as on the Codec screen"""
    w, h = im.size
    px = im.load()
    i, L = inset, length

    def put(x, y):
        if 0 <= x < w and 0 <= y < h:
            px[x, y] = col

    def tl():
        for k in range(L):
            put(i, i + 3 + k)
            put(i + 3 + k, i)
        put(i + 1, i + 2)
        put(i + 2, i + 1)

    def br():
        for k in range(L):
            put(w - i - 1, h - i - 3 - L + k)
            put(w - i - 3 - L + k, h - i - 1)
        put(w - i - 2, h - i - 3)
        put(w - i - 3, h - i - 2)

    def tr():
        for k in range(L):
            put(w - i - 1, i + 3 + k)
            put(w - i - 3 - L + k, i)
        put(w - i - 2, i + 2)
        put(w - i - 3, i + 1)

    def bl():
        for k in range(L):
            put(i, h - i - 3 - L + k)
            put(i + 3 + k, h - i - 1)
        put(i + 1, h - i - 3)
        put(i + 2, h - i - 2)

    tl()
    br()
    if which == "all":
        tr()
        bl()


def box(w, h, n, fill, edge, inner, corner, corner_len, grad_to=None, ticks=None):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    a, b = rgba(fill), rgba(grad_to or fill)
    fill_notch(im, n, lambda x, y: tuple(int(a[c] + (b[c] - a[c]) * y / max(1, h - 1)) for c in range(4)))
    stroke_notch(im, n, 0, rgba(edge))
    if inner:
        stroke_notch(im, n, 3 if w > 30 else 2, rgba(inner))
    if corner:
        brackets(im, corner_len, 0, rgba(corner), "all" if w > 30 else "diag")
    if ticks:
        px = im.load()
        for x in range(w // 2 - 2, w // 2 + 2):
            px[x, 1] = rgba(ticks)
            px[x, h - 2] = rgba(ticks)
    return im


def diamond(col, core):
    im = Image.new("RGBA", (9, 9), (0, 0, 0, 0))
    px = im.load()
    for y in range(9):
        half = 4 - abs(4 - y)
        for x in range(4 - half, 5 + half):
            px[x, y] = rgba(col)
    for x in range(3, 6):
        px[x, 4] = rgba(core)
    for y in range(3, 6):
        px[4, y] = rgba(core)
    return im


def background():
    im = Image.new("RGBA", (800, 480), rgba("#010806"))
    px = im.load()
    grid = rgba("#03140d")          # #2fd68a at 6% on the background
    for y in range(480):
        for x in range(800):
            if x % 40 == 0 or y % 40 == 0:
                px[x, y] = grid
            if y % 3 == 0:          # scanlines
                r, g, b, _ = px[x, y]
                px[x, y] = (r * 55 // 100, g * 55 // 100, b * 55 // 100, 255)
    return im


THEME = {
    "base": "Classic",
    "colors": {
        "bg": "#010806", "bar": "#04130d", "bar_border": "#1d6b4a", "strip": "#04130d", "strip_border": "#2fd68a",
        "dialog": "#020d09", "dialog_border": "#2fd68a",
        "btn": "#04130d", "btn_border": "#1d6b4a", "btn_text": "#5fd3a0",
        "btn_active": "#0b3424", "btn_active_border": "#6dffb9", "btn_active_text": "#e8fff3",
        "btn_page": "#2fd68a", "btn_page_text": "#021a10", "btn_off_text": "#1f5a42",
        "btn_mark": "#1d6b4a", "btn_mark_assigned": "#2fd68a", "btn_mark_active": "#eafff4",
        "text": "#bfffe0", "text_value": "#e8fff3", "text_dim": "#3d8f6b", "text_head": "#6dffb9",
        "accent": "#6dffb9", "line": "#1d6b4a",
        "sel_bg": "#0f4a33", "sel_text": "#eafff4", "sel_border": "#6dffb9", "focus": "#2fd68a", "focus_text": "#021a10",
        "ok": "#6dffb9", "warn": "#ffd166", "err": "#ff4136", "tx": "#ff4136", "tx_bg": "#3a0806", "new": "#ffd166",
        "mode_bg": "#2fd68a", "mode_text": "#021a10", "mode_border": "#6dffb9",
        "pill_off_text": "#2c7556", "pill_border": "#1d6b4a", "icon_off": "#1f5a42",
        "meter_weak": "#1d6b4a", "meter_bar": "#6dffb9", "meter_over": "#ff4136",
        "vfo_line": "#ff4136", "spot": "#d6ffe9",
        "bp_cw": "#0f4a36", "bp_dig": "#176b4b", "bp_ssb": "#1f8a5f", "bp_bcn": "#6b2a24", "bp_text": "#bfffe0",
        "off": "#12382a",
        "bar_grad": "#020c08",
        "strip_grad": "#020b07",
        "dialog_grad": "#020d09",
        "btn_grad": "#04130d",
        "btn_active_grad": "#0b3424",
        "btn_off_border": "#0f3125",
        "card": "#1d6b4a",
        "edited": "#ffd166",
        "panel": "#020d09",
        "panel_border": "#1d6b4a",
        "list": "#031009",
        "row": "#062016",
        "popup": "#04130d",
        "popup_border": "#2fd68a",
        "side": "#04130d",
        "chip": "#0b3424",
        "focus_bg": "#0f4a33",
        "near_bg": "#0b3a2a",
        "vfo": "#9dffcf",
        "vfo_bg": "#06281b",
        "text_soft": "#8fdcb6",
        "text_mute": "#3d8f6b",
        "text_faint": "#2c6a4f",
        "action": "#ffd166",
        "accent_soft": "#ffd166",
        "title_text": "#021a10",
        "ok_bg": "#06281b",
        "ok_bar": "#2fd68a",
        "warn_bg": "#2a2205",
        "err_strong": "#ff2a1a",
        "err_bg": "#3a0806",
        "info": "#9dffcf",
        "digi": "#c8ff6d",
        "alert": "#ffd166",
        "alert_text": "#ffe39a",
        "alert_bg": "#3a2c06",
        "veil": "#010806",
        "alert_box": "#2a2005",
        "track": "#0b3424",
        "frame": "#031009",
        "switch_off": "#3d8f6b",
        "lock": "#ffd166",
        "control_border": "#2fd68a",
        "toggle_on": "#2fd68a",
        "toggle_focus": "#9dffcf",
        "mode_dig": "#c8ff6d",
        "meter_low": "#6dffb9",
        "meter_high": "#ff4136",
        "bar_high": "#ff4136",
        "bar_mid": "#ffd166",
        "alc": "#9dffcf",
        "tx_box": "#3a0806",
        "tune_good": "#eafff4",
        "finder_band": "#ff4136",
        "finder_cursor": "#6dffb9",
        "pill_hp": "#2a2205",
        "pill_cat": "#06281b",
        "pill_cat_text": "#9dffcf",
        "pill_kbd": "#0b3424",
        "pill_kbd_text": "#c8ff6d",
        "age_mid": "#ffd166",
        "age_old": "#b08a3c",
        "row_alert": "#3a2c06",
        "row_alert_text": "#ffe39a",
        "row_heard": "#0b3424",
        "row_tx": "#3a0806",
        "row_to_me": "#7a1208",
        "row_cq": "#0f4a33",
        "row_group": "#06281b",
        "row_sel_bar": "#6dffb9",
        "ft8_cq": "#2fd68a",
        "ft8_cq_worked": "#1d6b4a",
        "ft8_cq_same": "#12382a",
        "ft8_to_me": "#ff4136",
        "ft8_tx": "#ffd166",
    },
    "radius": 0, "radius_btn": 0, "radius_popup": 0, "border": 1,
    "btn_gradient": False, "box_gradient": False, "focus_solid": True,
    "font": "codec",
    "waterfall": ["#000604", "#00150c", "#00341f", "#06633a", "#1fa468", "#6fe8ad", "#d8fff0", "#ffffff"],
    "waterfall_steps": 0,
    "mark": [6, 9, 9], "pill_border": 1, "mode_box": True, "btn_gap": 1,
    "codec": True,
    "images": {
        "btn": {"file": "btn.png", "slice": [7, 5, 7, 4]},
        "btn_active": {"file": "btn_active.png", "slice": [7, 5, 7, 4]},
        "btn_off": {"file": "btn_off.png", "slice": [7, 5, 7, 4]},
        "btn_page": {"file": "btn_page.png", "slice": [7, 5, 7, 4]},
        "bar": {"file": "bar.png", "slice": [14, 14, 14, 14]},
        "strip": {"file": "strip.png", "slice": [14, 14, 14, 14]},
        "dialog": {"file": "panel.png", "slice": [12, 12, 12, 12]},
        "bg": {"file": "background.png", "slice": [0, 0, 0, 0]},
        "mark": {"file": "mark.png"},
        "mark_active": {"file": "mark_active.png"},
    },
}


def main():
    os.makedirs(OUT, exist_ok=True)
    images = {
        "bar.png": box(40, 40, 4, "#03110b", "#1d6b4a", "#0f3a29", "#6dffb9", 7),
        "strip.png": box(40, 40, 5, "#062016", "#2fd68a", "#14523a", "#eafff4", 8, grad_to="#020b07", ticks="#6dffb9"),
        "panel.png": box(40, 40, 6, "#020d09", "#2fd68a", "#1d6b4a", "#eafff4", 6),
        "btn.png": box(65, 23, 3, "#04130d", "#1d6b4a", None, "#5fd3a0", 3),
        "btn_active.png": box(65, 23, 3, "#0b3424", "#6dffb9", "#1f8a5f", "#eafff4", 3),
        "btn_off.png": box(65, 23, 3, "#020a07", "#0f3125", None, None, 0),
        "btn_page.png": box(65, 23, 3, "#2fd68a", "#9dffcf", "#1fa468", "#eafff4", 3),
        "mark.png": diamond("#1d6b4a", "#2fd68a"),
        "mark_active.png": diamond("#6dffb9", "#ffffff"),
        "background.png": background(),
    }
    for name, im in images.items():
        im.save(os.path.join(OUT, name), optimize=True)
    with open(os.path.join(OUT, "theme.json"), "w") as f:
        json.dump(THEME, f, indent=2)
        f.write("\n")
    print("Codec/:", ", ".join(sorted(images)), "and theme.json")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Ops Room: an amber operations-room theme, all drawn here (nothing taken from anywhere): orange
type on black, frames cut at two corners, hazard stripes, a faint hex grid, red for transmit, violet
and green for what is lit.

    python3 make_ops_room.py       # writes Ops Room/: theme.json and the 9-slice PNGs

Copy Ops Room/ to the SD card as /mnt/themes/Ops Room/. Its font is Roboto Condensed
("font": "rcond"). Format: docs/THEMES.md. Needs Pillow.
"""
import json
import math
import os

from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Ops Room")

ORANGE, AMBER, PALE, EMBER, RED = "#ff8c1a", "#ffb347", "#ffe2b8", "#a8520f", "#ff2b1c"
VIOLET, GREEN, BLACK = "#8c5cff", "#7dff5a", "#050403"


def rgba(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def mix(a, b, t):
    a, b = rgba(a), rgba(b)
    return tuple(int(a[c] + (b[c] - a[c]) * t + 0.5) for c in range(4))


def cut_inside(w, h, tl, br):
    """Inside a rectangle with its top left and bottom right corners cut at 45 degrees"""
    return lambda x, y: x + y >= tl and (w - 1 - x) + (h - 1 - y) >= br


def frame(w, h, tl, br, fill, edge, width=1, grad_to=None, inner=None, hazard=None, tab=None, tick=None):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = im.load()
    out = cut_inside(w, h, tl, br)
    for y in range(h):
        for x in range(w):
            if not out(x, y):
                continue
            # the edge: within width of the outline, the cut corners included
            d = min(x, y, w - 1 - x, h - 1 - y, x + y - tl, (w - 1 - x) + (h - 1 - y) - br)
            if d < width:
                px[x, y] = rgba(edge)
            elif inner and d == width + 2:
                px[x, y] = rgba(inner)
            else:
                px[x, y] = mix(fill, grad_to or fill, y / max(1, h - 1))
    if hazard:              # stripes in the bottom left corner: fixed, the slice does not stretch them
        hx, hy, hw, hh = hazard
        for y in range(hy, hy + hh):
            for x in range(hx, hx + hw):
                px[x, y] = rgba(ORANGE) if ((x + y) // 3) % 2 == 0 else rgba(BLACK)
    if tab:                 # a bar of colour along the top, from the cut corner
        tx, tw, th, col = tab
        for y in range(0, th):
            for x in range(tx, tx + tw):
                if out(x, y):
                    px[x, y] = rgba(col)
    if tick:                # a short bright mark at the middle of the top edge
        for x in range(w // 2 - 3, w // 2 + 3):
            px[x, 0] = rgba(tick)
            px[x, 1] = rgba(tick)
    return im


# A 5 x 7 stencil font for the labels baked into the frames (only the letters used)
GLYPH = {
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "0": ["01110", "10011", "10101", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00110", "01000", "10000", "11111"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
}


def label(im, x, y, s, col):
    px = im.load()
    for ch in s:
        for r, row in enumerate(GLYPH.get(ch, ["00000"] * 7)):
            for c, b in enumerate(row):
                if b == "1":
                    px[x + c, y + r] = rgba(col)
        x += 6


def plate(im, x, y, s, fill=None, ink=BLACK):
    """A label on a tab of colour: 2 px around the letters"""
    fill = fill or ORANGE
    px = im.load()
    w = len(s) * 6 + 3
    for yy in range(y, y + 10):
        for xx in range(x, x + w):
            px[xx, yy] = rgba(fill)
    label(im, x + 2, y + 1, s, ink)


def stripes(im, x, y, w, h, col=ORANGE, period=6):
    px = im.load()
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            px[xx, yy] = rgba(col) if ((xx - yy) // (period // 2)) % 2 == 0 else rgba(BLACK)


def hexagon(col, core):
    im = Image.new("RGBA", (9, 9), (0, 0, 0, 0))
    px = im.load()
    rows = [(2, 6), (1, 7), (0, 8), (0, 8), (0, 8), (0, 8), (0, 8), (1, 7), (2, 6)]
    for y, (a, b) in enumerate(rows):
        for x in range(a, b + 1):
            px[x, y] = rgba(col)
    for y in range(3, 6):
        for x in range(3, 6):
            px[x, y] = rgba(core)
    return im


def background():
    """A faint honeycomb on black, 24 px across, and a slightly darker line every fourth row"""
    im = Image.new("RGBA", (800, 480), rgba(BLACK))
    px = im.load()
    line = rgba("#170c03")
    r = 14.0
    for y in range(480):
        for x in range(800):
            # distance to the nearest hexagon edge of a pointy-top grid
            q = (x * math.sqrt(3) / 3 - y / 3) / r
            s = (y * 2 / 3) / r
            # cube rounding
            cx, cz = q, s
            cy = -cx - cz
            rx, ry, rz = round(cx), round(cy), round(cz)
            dx, dy, dz = abs(rx - cx), abs(ry - cy), abs(rz - cz)
            if dx > dy and dx > dz:
                rx = -ry - rz
            elif dy > dz:
                ry = -rx - rz
            else:
                rz = -rx - ry
            hxc = r * math.sqrt(3) * (rx + rz / 2)
            hyc = r * 1.5 * rz
            vx, vy = abs(x - hxc), abs(y - hyc)
            edge = max(vx, vx / 2 + vy * math.sqrt(3) / 2)     # the hex's own distance: its edge at r sqrt(3) / 2
            if edge > r * math.sqrt(3) / 2 - 1.0:
                px[x, y] = line
            if y % 4 == 0:
                rr, gg, bb, _ = px[x, y]
                px[x, y] = (rr * 7 // 10, gg * 7 // 10, bb * 7 // 10, 255)
    return im


C = {
    "bg": BLACK, "bar": "#0d0703", "bar_grad": "#090502", "bar_border": EMBER,
    "strip": "#160a04", "strip_grad": "#0a0502", "strip_border": ORANGE,
    "dialog": "#0a0603", "dialog_grad": "#0a0603", "dialog_border": ORANGE,
    "btn": "#0f0804", "btn_grad": "#0f0804", "btn_border": EMBER, "btn_text": ORANGE,
    "btn_active": "#3a1a05", "btn_active_grad": "#3a1a05", "btn_active_border": AMBER, "btn_active_text": PALE,
    "btn_page": ORANGE, "btn_page_text": "#140700", "btn_off_text": "#4a2a0c", "btn_off_border": "#2a1606",
    "btn_mark": "#5a300c", "btn_mark_assigned": ORANGE, "btn_mark_active": PALE,
    "text": "#ffc27a", "text_value": PALE, "text_soft": "#e8a35c", "text_dim": "#9a5a1e", "text_mute": "#7a4616",
    "text_faint": "#4a2a0c", "text_head": ORANGE, "accent": AMBER, "accent_soft": VIOLET, "line": EMBER,
    "title_text": "#140700",
    "action": GREEN,
    "sel_bg": "#4a2306", "sel_text": PALE, "sel_border": AMBER, "focus": ORANGE, "focus_text": "#140700",
    "focus_bg": "#4a2306", "near_bg": "#2a1404",
    "ok": GREEN, "ok_bg": "#0f2a08", "ok_bar": GREEN, "warn": "#ffd23a", "warn_bg": "#2a2005",
    "err": RED, "err_strong": "#ff1a0a", "err_bg": "#3a0806", "tx": RED, "tx_bg": "#3a0806", "tx_box": "#3a0806",
    "info": VIOLET, "digi": GREEN, "new": "#ffd23a",
    "alert": RED, "alert_text": "#ffd0c8", "alert_bg": "#3a0806", "alert_box": "#2a0604",
    "mode_bg": ORANGE, "mode_text": "#140700", "mode_border": AMBER, "mode_dig": GREEN,
    "pill_off_text": "#5a3410", "pill_border": EMBER, "icon_off": "#4a2a0c",
    "meter_weak": "#5a300c", "meter_bar": ORANGE, "meter_low": ORANGE, "meter_high": RED, "meter_over": RED,
    "bar_high": RED, "bar_mid": "#ffd23a", "alc": VIOLET,
    "vfo_line": RED, "vfo": VIOLET, "vfo_bg": "#1c1240", "spot": PALE, "tune_good": GREEN,
    "bp_cw": "#3b2470", "bp_dig": "#1f5a16", "bp_ssb": "#7a3a08", "bp_bcn": "#7a1010", "bp_text": PALE,
    "finder_band": RED, "finder_cursor": GREEN,
    "off": "#2a1606", "card": EMBER, "edited": GREEN, "panel": "#0a0603", "panel_border": EMBER,
    "list": "#080502", "row": "#160a04", "popup": "#0d0703", "popup_border": ORANGE, "side": "#0d0703",
    "chip": "#1d0e04", "veil": BLACK, "track": "#24120a", "frame": "#080502",
    "switch_off": "#7a4616", "lock": "#ffd23a", "control_border": ORANGE, "toggle_on": ORANGE, "toggle_focus": AMBER,
    "pill_hp": "#2a2005", "pill_cat": "#1c1240", "pill_cat_text": "#c0a8ff", "pill_kbd": "#0f2a08",
    "pill_kbd_text": GREEN,
    "age_mid": "#ffd23a", "age_old": "#a8520f",
    "row_alert": "#3a0806", "row_alert_text": "#ffd0c8", "row_heard": "#1d0e04", "row_tx": "#3a0806",
    "row_to_me": "#7a1208", "row_cq": "#2a1404", "row_group": "#1c1240", "row_sel_bar": ORANGE,
    "ft8_cq": GREEN, "ft8_cq_worked": "#3a6a28", "ft8_cq_same": "#1f3a14", "ft8_to_me": RED, "ft8_tx": "#ffd23a",
}

THEME = {
    "base": "Classic",
    "colors": C,
    "radius": 0, "radius_btn": 0, "radius_popup": 0, "border": 1,
    "btn_gradient": False, "box_gradient": False, "focus_solid": True,
    "font": "rcond",
    "waterfall": ["#000000", "#12021e", "#3a0a4a", "#7a1030", "#d0301a", "#ff7a10", "#ffd060", "#ffffff"],
    "waterfall_steps": 0,
    "mark": [6, 9, 9], "pill_border": 1, "mode_box": True, "btn_gap": 1,
    "ops_room": True,
    "images": {
        "btn": {"file": "btn.png", "slice": [7, 5, 7, 5]},
        "btn_active": {"file": "btn_active.png", "slice": [7, 5, 7, 5]},
        "btn_off": {"file": "btn_off.png", "slice": [7, 5, 7, 5]},
        "btn_page": {"file": "btn_page.png", "slice": [7, 5, 7, 5]},
        "bar": {"file": "bar.png", "slice": [24, 12, 12, 12]},
        "strip": {"file": "strip.png", "slice": [44, 14, 40, 14]},
        "dialog": {"file": "panel.png", "slice": [52, 14, 40, 14]},
        "bg": {"file": "background.png", "slice": [0, 0, 0, 0]},
        "mark": {"file": "mark.png"},
        "mark_active": {"file": "mark_active.png"},
    },
}


def main():
    os.makedirs(OUT, exist_ok=True)
    strip = frame(120, 40, 10, 10, "#160a04", ORANGE, width=2, grad_to="#0a0502", inner="#3a1c06",
                  hazard=(3, 30, 14, 6))
    plate(strip, 12, 0, "FREQ")
    stripes(strip, 84, 35, 22, 3, RED)
    panel = frame(120, 40, 10, 10, "#0a0603", ORANGE, inner="#2a1606", hazard=(3, 31, 14, 5))
    plate(panel, 12, 0, "SYS-01")
    stripes(panel, 86, 1, 22, 4, ORANGE)
    bar = frame(60, 40, 7, 7, "#0d0703", EMBER, grad_to="#090502", tab=(7, 12, 3, ORANGE))
    images = {
        "bar.png": bar,
        "strip.png": strip,
        "panel.png": panel,
        "btn.png": frame(65, 23, 5, 5, "#0f0804", EMBER),
        "btn_active.png": frame(65, 23, 5, 5, "#3a1a05", AMBER, inner="#6a300a"),
        "btn_off.png": frame(65, 23, 5, 5, "#070402", "#2a1606"),
        "btn_page.png": frame(65, 23, 5, 5, ORANGE, "#ffd08a", grad_to="#e8700a"),
        "mark.png": hexagon("#5a300c", ORANGE),
        "mark_active.png": hexagon(AMBER, "#ffffff"),
        "background.png": background(),
    }
    for name, im in images.items():
        im.save(os.path.join(OUT, name), optimize=True)
    with open(os.path.join(OUT, "theme.json"), "w") as f:
        json.dump(THEME, f, indent=2)
        f.write("\n")
    print("Ops Room/:", ", ".join(sorted(images)), "and theme.json")


if __name__ == "__main__":
    main()

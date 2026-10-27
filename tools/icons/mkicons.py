#!/usr/bin/env python3
"""The icons of the icon menus (APP grid, Modes grid, the Settings rail): white line drawings on
a transparent ground, recoloured by the GUI to the theme's accent (theme_icon_img). Drawn here with
PIL at four times the size and scaled down, so they stay crisp at 24 and 36 px.

  python3 tools/icons/mkicons.py rootfs/usr/share/x6100/icons

Also a 1-bit set in <out>/1bit/ for the 1bit theme: the same drawings scaled down by area and cut
at half coverage, every pixel on or off with strokes of even weight (thresholding the Lanczos
ones left them uneven).

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import math
import os
import sys

from PIL import Image, ImageDraw

SIZES = (24, 36)   # the Settings rail, the APP and Modes grids
SS = 4
WHITE = (255, 255, 255, 255)


class Pen:
    """Drawing on a 100 x 100 canvas, whatever the output size."""

    def __init__(self, size):
        self.s = size * SS
        self.im = Image.new("RGBA", (self.s, self.s), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)
        self.w = max(2, int(self.s * 0.085))   # the stroke

    def p(self, x, y):
        return (x * self.s / 100, y * self.s / 100)

    def line(self, pts, w=None):
        self.d.line([self.p(x, y) for x, y in pts], fill=WHITE, width=w or self.w, joint="curve")
        for x, y in (pts[0], pts[-1]):     # round ends
            self.dot(x, y, (w or self.w) / 2 * 100 / self.s)

    def dot(self, x, y, r):
        cx, cy = self.p(x, y)
        rr = r * self.s / 100
        self.d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=WHITE)

    def ring(self, x, y, r, w=None, start=0, end=360):
        cx, cy = self.p(x, y)
        rr = r * self.s / 100
        self.d.arc([cx - rr, cy - rr, cx + rr, cy + rr], start=start, end=end, fill=WHITE, width=w or self.w)

    def box(self, x0, y0, x1, y1, r=8, w=None):
        self.d.rounded_rectangle([self.p(x0, y0), self.p(x1, y1)], radius=r * self.s / 100, outline=WHITE, width=w or self.w)

    def fill_box(self, x0, y0, x1, y1, r=4):
        self.d.rounded_rectangle([self.p(x0, y0), self.p(x1, y1)], radius=r * self.s / 100, fill=WHITE)

    def poly(self, pts):
        self.d.polygon([self.p(x, y) for x, y in pts], fill=WHITE)

    def out(self, size):
        return self.im.resize((size, size), Image.LANCZOS)

    def out_1bit(self, size):
        alpha = self.im.getchannel("A").resize((size, size), Image.BOX).point(lambda v: 255 if v >= 128 else 0)
        im = Image.new("RGBA", (size, size), (255, 255, 255, 0))
        im.putalpha(alpha)
        return im


def sine(pen, x0, x1, y, amp, periods, w=None):
    pts = [(x0 + (x1 - x0) * i / 60, y - amp * math.sin(i / 60 * 2 * math.pi * periods)) for i in range(61)]
    pen.line(pts, w)


# ---- the icons, by name ---------------------------------------------------------------------

def i_modes(p):      # a sine through a keyboard-ish base: the digital modes
    sine(p, 10, 90, 40, 18, 1.5)
    p.line([(18, 78), (82, 78)])

def i_cluster(p):    # a globe
    p.ring(50, 50, 36)
    p.ring(50, 50, 36, start=0, end=360)
    p.line([(14, 50), (86, 50)])
    p.d.ellipse([p.p(32, 14), p.p(68, 86)], outline=WHITE, width=p.w)
    p.line([(50, 14), (50, 86)])

def i_field(p):      # mountains and a sun
    p.line([(8, 82), (34, 40), (50, 64), (62, 48), (92, 82), (8, 82)])
    p.ring(74, 26, 9)

def i_bands(p):      # bars of activity
    for i, h in enumerate((30, 55, 42, 70, 48)):
        x = 14 + i * 18
        p.line([(x, 84), (x, 84 - h)], w=int(p.w * 1.3))

def i_channels(p):   # a list with a marked line
    for y in (26, 50, 74):
        p.line([(34, y), (86, y)])
    p.dot(18, 26, 6); p.dot(18, 50, 6); p.dot(18, 74, 6)

def i_swr(p):        # a gauge
    p.ring(50, 60, 36, start=180, end=360)
    p.line([(50, 60), (72, 36)])
    p.dot(50, 60, 6)
    p.line([(14, 60), (14, 70)], w=p.w // 2); p.line([(86, 60), (86, 70)], w=p.w // 2)

def i_recorder(p):   # a microphone
    p.d.rounded_rectangle([p.p(38, 10), p.p(62, 54)], radius=12 * p.s / 100, outline=WHITE, width=p.w)
    p.ring(50, 46, 24, start=0, end=180)
    p.line([(50, 70), (50, 86)]); p.line([(34, 86), (66, 86)])

def i_trainer(p):    # a Morse key: dit dah dah dit
    p.dot(16, 50, 6)
    p.line([(32, 50), (50, 50)])
    p.line([(60, 50), (78, 50)])
    p.dot(90, 50, 6)
    p.line([(16, 76), (90, 76)], w=p.w // 2)

def i_gps(p):        # a map pin
    p.ring(50, 38, 24, start=150, end=390)
    p.line([(29, 50), (50, 88)]); p.line([(71, 50), (50, 88)])
    p.ring(50, 38, 8)

def i_screenrec(p):  # a monitor with a red-dot place
    p.box(10, 16, 90, 70, r=6)
    p.line([(36, 86), (64, 86)]); p.line([(50, 70), (50, 86)])
    p.dot(50, 43, 10)

def i_walkie(p):     # a handheld
    p.box(30, 24, 70, 90, r=8)
    p.line([(40, 8), (40, 24)])
    p.line([(42, 40), (58, 40)])
    p.dot(50, 66, 7)

def i_settings(p):   # three sliders
    for i, (y, x) in enumerate(((26, 36), (50, 64), (74, 44))):
        p.line([(14, y), (86, y)])
        p.dot(x, y, 8)

def i_general(p):    # an id card: the station
    p.box(8, 24, 92, 76, r=6)
    p.ring(30, 50, 9)
    p.line([(50, 42), (80, 42)]); p.line([(50, 58), (80, 58)])

def i_display(p):    # a screen with a waveform
    p.box(10, 18, 90, 68, r=6)
    sine(p, 20, 80, 43, 10, 1.5, w=p.w // 2 + 1)
    p.line([(36, 84), (64, 84)])

def i_audio(p):      # a speaker
    p.poly([(14, 40), (34, 40), (54, 22), (54, 78), (34, 60), (14, 60)])
    p.ring(54, 50, 20, start=-45, end=45)
    p.ring(54, 50, 32, start=-45, end=45)

def i_wireless(p):   # wifi arcs
    for r in (16, 30, 44):
        p.ring(50, 70, r, start=215, end=325)
    p.dot(50, 74, 6)

def i_bluetooth(p):  # the rune
    p.line([(50, 8), (50, 92)])
    p.line([(50, 8), (74, 30), (26, 70)])
    p.line([(50, 92), (74, 70), (26, 30)])

def i_keys(p):       # a paddle, two levers
    p.line([(14, 50), (56, 50)])
    p.ring(70, 50, 14)
    p.line([(56, 36), (56, 64)], w=int(p.w * 1.4))

def i_txcal(p):      # a tuning scale with a pointer
    p.line([(10, 70), (90, 70)])
    for x in (20, 35, 50, 65, 80):
        p.line([(x, 70), (x, 58 if x == 50 else 64)], w=p.w // 2 + 1)
    p.poly([(50, 44), (41, 24), (59, 24)])

def i_info(p):       # a circle with an i
    p.ring(50, 50, 38)
    p.dot(50, 32, 6)
    p.line([(50, 46), (50, 70)])

def i_ft(p):         # a clock: the timed modes
    p.ring(50, 50, 38)
    p.line([(50, 26), (50, 50), (68, 62)])

def i_js8(p):        # a speech bubble
    p.box(10, 14, 90, 68, r=12)
    p.line([(30, 68), (26, 88), (48, 68)])
    for x in (34, 50, 66):
        p.dot(x, 41, 5)

def i_wspr(p):       # a beacon
    p.line([(50, 92), (50, 46)])
    p.dot(50, 42, 7)
    for r in (22, 36):
        p.ring(50, 42, r, start=200, end=340)

def i_text(p):       # a keyboard
    p.box(8, 28, 92, 72, r=6)
    for y in (42, 56):
        for x in (22, 36, 50, 64, 78):
            p.dot(x, y, 3.5)

def i_image(p):      # a picture
    p.box(10, 16, 90, 84, r=6)
    p.line([(16, 76), (40, 48), (56, 64), (68, 54), (84, 76)])
    p.dot(68, 34, 7)

def i_birdie(p):     # a spike over the noise: a birdie
    p.line([(8, 78), (24, 72), (36, 76), (48, 70), (56, 74), (64, 68), (76, 74), (92, 70)], w=p.w // 2 + 1)
    p.line([(60, 74), (60, 16)])
    p.dot(60, 14, 6)

def i_navtex(p):     # lines of text
    p.box(16, 10, 84, 90, r=6)
    for y in (32, 48, 64):
        p.line([(30, y), (70 if y != 64 else 56, y)], w=p.w // 2 + 1)


ICONS = {n[2:]: f for n, f in globals().items() if n.startswith("i_") and callable(f)}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "rootfs/usr/share/x6100/icons"
    os.makedirs(os.path.join(out, "1bit"), exist_ok=True)
    for name, draw in sorted(ICONS.items()):
        for size in SIZES:
            pen = Pen(size)
            draw(pen)
            pen.out(size).save(os.path.join(out, f"{name}_{size}.png"))
            pen.out_1bit(size).save(os.path.join(out, "1bit", f"{name}_{size}.png"))
    print(f"{len(ICONS)} icons x {len(SIZES)} sizes in {out}, and in {out}/1bit")


if __name__ == "__main__":
    main()

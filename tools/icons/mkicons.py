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

def i_mail(p):       # an envelope: Winlink mail
    p.box(8, 22, 92, 78, r=6)
    p.line([(12, 28), (50, 56), (88, 28)])

def i_navtex(p):     # lines of text
    p.box(16, 10, 84, 90, r=6)
    for y in (32, 48, 64):
        p.line([(30, y), (70 if y != 64 else 56, y)], w=p.w // 2 + 1)

def i_prop(p):       # a sky wave: up from the ground, bent back by the ionosphere, under a sun
    p.line([(8, 84), (92, 84)])
    p.line([(16, 84), (34, 44), (50, 32), (66, 44), (84, 84)], w=p.w // 2 + 2)
    p.ring(78, 18, 8)

def i_scan(p):       # a sweep through the channels, stopped on one
    for i, h in enumerate((14, 20, 12, 46, 16, 10)):
        x = 14 + i * 14.4
        p.line([(x, 82), (x, 82 - h)], w=p.w // 2 + 1)
    p.line([(57, 22), (57, 88)])
    p.poly([(49, 12), (65, 12), (57, 24)])

def i_aero(p):       # an aeroplane from above with radio waves: the HF air band
    p.poly([(30, 30), (36, 34), (36, 52), (58, 64), (58, 70), (36, 62), (36, 76), (44, 84), (44, 88),
            (30, 84), (16, 88), (16, 84), (24, 76), (24, 62), (2, 70), (2, 64), (24, 52), (24, 34)])
    for r in (16, 28):
        p.ring(66, 36, r, w=p.w // 2 + 1, start=-60, end=60)

def i_cb(p):         # a mobile set with its hand microphone: CB
    p.box(6, 30, 70, 64, r=6)
    p.dot(24, 47, 7)
    p.line([(42, 42), (58, 42)], w=p.w // 2 + 1)
    p.line([(42, 53), (58, 53)], w=p.w // 2 + 1)
    p.line([(70, 47), (83, 56), (83, 68)], w=p.w // 2 + 1)
    p.fill_box(73, 68, 93, 94, r=8)

def i_flights(p):    # an aircraft seen from above, nose up: the Flights app
    p.line([(50, 8), (57, 18), (57, 38), (92, 58), (92, 66), (57, 56), (57, 76), (70, 86), (70, 92), (50, 87),
            (30, 92), (30, 86), (43, 76), (43, 56), (8, 66), (8, 58), (43, 38), (43, 18), (50, 8)], w=p.w // 2 + 2)

def i_iradio(p):     # a radio set with its antenna and a wave: the Internet radio (the CX8ANM edition's drawing)
    p.box(10, 40, 90, 86, r=8)
    p.ring(36, 63, 11)
    for y in (56, 70):
        p.line([(58, y), (78, y)], w=p.w // 2 + 1)
    p.line([(30, 40), (70, 12)], w=p.w // 2 + 1)
    p.ring(74, 12, 8, w=p.w // 2 + 1, start=200, end=340)

def i_zello(p):      # a cloud with a voice in it: talk over the internet (the Zello app; not the service's logo)
    p.ring(26, 56, 16, start=90, end=270)
    p.ring(46, 38, 20, start=180, end=320)
    p.ring(72, 54, 18, start=235, end=90)
    p.line([(26, 72), (72, 72)])
    for x, h in ((38, 7), (50, 14), (62, 5)):
        p.line([(x, 52 - h), (x, 52 + h)], w=p.w // 2 + 1)

def i_svx(p):        # a reflector: stations around a hub, each linked to it (the SVXConnect app)
    p.ring(50, 50, 12)
    for x, y in ((16, 18), (84, 18), (16, 82), (84, 82)):
        dx, dy = 50 - x, 50 - y
        k = 12 / math.hypot(dx, dy)
        p.line([(x + dx * 0.2, y + dy * 0.2), (50 - dx * k, 50 - dy * k)], w=p.w // 2 + 1)
        p.dot(x, y, 7)

def i_dmr(p):        # digital voice heard: a pulse train with the arcs of sound over it (the DMR app, listen only)
    p.line([(6, 84), (6, 68), (24, 68), (24, 84), (42, 84), (42, 68), (60, 68), (60, 84), (78, 84), (78, 68),
            (94, 68)], w=p.w // 2 + 1)
    p.dot(50, 46, 7)
    for r in (17, 31):
        p.ring(50, 46, r, w=p.w // 2 + 1, start=205, end=335)


def i_meteo(p):      # a sun behind a cloud: the Weather app
    p.ring(66, 30, 13)
    for a in (200, 245, 290, 335, 20):
        c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
        p.line([(66 + 20 * c, 30 + 20 * s), (66 + 26 * c, 30 + 26 * s)])
    # the cloud: the outline of three rounds on a flat base, the sun hidden behind it
    lobes, base = ((24, 68, 14), (46, 56, 18), (66, 68, 14)), (24, 58, 66, 82)
    for grow, fill in ((p.w / 2, WHITE), (-p.w / 2, (0, 0, 0, 0))):
        for x, y, r in lobes:
            (cx, cy), rr = p.p(x, y), r * p.s / 100 + grow
            p.d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=fill)
        (x0, y0), (x1, y1) = p.p(base[0], base[1]), p.p(base[2], base[3])
        p.d.rectangle([x0, y0 - grow, x1, y1 + grow], fill=fill)


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

#!/usr/bin/env python3
"""Field Ops home tiles, drawn with PIL at four times the size and scaled down, beside the SOTA, POTA,
WWBOTA, HOTA, Bands and Log icons the project was forked with:

  wwff_96.png, wwff_64.png   WWFF, a leaf in the tile's blue
  tota_96.png, tota_64.png   TOTA, a lookout tower in the tile's orange: lattice legs, railed platform, roof, pennant
  wca_96.png,  wca_64.png    WCA, a grey stone castle: two crenellated towers, a crenellated wall with a gate
  mota_96.png, mota_64.png   MOTA, a windmill in the tile's teal with four light lattice sails

  python3 tools/icons/mkfield.py rootfs/usr/share/x6100/field

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import math
import os
import sys

from PIL import Image, ImageDraw

SS = 4
LEAF = (79, 139, 245, 255)      # the tile's accent (theme role vfo)
LEAF_DARK = (46, 98, 196, 255)
STEM = (46, 98, 196, 255)
TOWER = (250, 152, 74, 255)     # the TOTA tile's accent (orange)
TOWER_DARK = (196, 104, 40, 255)
TOWER_LIGHT = (255, 214, 170, 255)
STONE = (203, 213, 225, 255)    # the WCA tile's accent (theme role text_soft)
STONE_MID = (165, 177, 194, 255)
STONE_DARK = (120, 132, 150, 255)
MILL = (65, 175, 199, 255)      # the MOTA tile's accent (teal)
MILL_DARK = (36, 120, 140, 255)
SAIL = (235, 240, 245, 255)
SAIL_EDGE = (150, 160, 175, 255)


def canvas(size):
    """A transparent square SS times the size, drawn on a 100 x 100 grid: p() places a point, u() sizes a width."""
    s = size * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im), lambda x, y: (x * s / 100, y * s / 100), lambda n: max(1, round(n * s / 100))


def leaf(size):
    s = size * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    p = lambda x, y: (x * s / 100, y * s / 100)
    w = max(2, int(s * 0.05))

    # The blade: two arcs meeting at the tip (top right) and the base (bottom left)
    pts = []
    for i in range(0, 61):
        t = i / 60
        # left/upper edge
        x = 14 + 72 * t
        y = 86 - 72 * t - 30 * math.sin(math.pi * t)
        pts.append(p(x, y))
    for i in range(60, -1, -1):
        t = i / 60
        x = 14 + 72 * t
        y = 86 - 72 * t + 30 * math.sin(math.pi * t)
        pts.append(p(x, y))
    d.polygon(pts, fill=LEAF)
    # The midrib and veins
    d.line([p(14, 86), p(86, 14)], fill=LEAF_DARK, width=w)
    for t in (0.3, 0.5, 0.7):
        x = 14 + 72 * t
        y = 86 - 72 * t
        d.line([p(x, y), p(x + 10, y + 5)], fill=LEAF_DARK, width=w // 2 + 1)    # to the lower edge
        d.line([p(x, y), p(x - 5, y - 10)], fill=LEAF_DARK, width=w // 2 + 1)    # to the upper edge
    # The stem
    d.line([p(14, 86), p(6, 96)], fill=STEM, width=w + 2)
    return im.resize((size, size), Image.LANCZOS)


def tower(size):
    im, d, p, u = canvas(size)
    top, foot = 52, 94                              # the lattice shaft
    lx = lambda y: 37 - 14 * (y - top) / (foot - top)    # its left leg; the right one mirrors it

    # The shaft: cross braces between the legs, three bays of diagonals, the legs over them
    bays = (top, 65, 79, foot)
    for y0, y1 in zip(bays, bays[1:]):
        d.line([p(lx(y0), y0), p(100 - lx(y1), y1)], fill=TOWER_DARK, width=u(3.5))
        d.line([p(100 - lx(y0), y0), p(lx(y1), y1)], fill=TOWER_DARK, width=u(3.5))
    for y in bays[1:-1]:
        d.line([p(lx(y), y), p(100 - lx(y), y)], fill=TOWER, width=u(3.5))
    for x0, x1 in ((lx(top), lx(foot)), (100 - lx(top), 100 - lx(foot))):
        d.polygon([p(x0 - 3, top), p(x0 + 3, top), p(x1 + 3, foot), p(x1 - 3, foot)], fill=TOWER)
    d.rectangle([p(lx(foot) - 6, foot - 2), p(100 - lx(foot) + 6, foot + 1)], fill=TOWER_DARK)    # footings

    # The platform: deck, railing, the roof on two posts
    d.rectangle([p(23, 47), p(77, 52)], fill=TOWER_DARK)
    d.rectangle([p(25, 38), p(75, 41.5)], fill=TOWER)                      # handrail
    for x in (26.5, 34.5, 42.5, 50, 57.5, 65.5, 73.5):
        d.rectangle([p(x - 1.5, 41), p(x + 1.5, 47)], fill=TOWER)          # balusters
    for x in (31, 69):
        d.rectangle([p(x - 2, 28), p(x + 2, 38)], fill=TOWER)              # roof posts
    d.polygon([p(50, 13), p(75, 29.5), p(25, 29.5)], fill=TOWER)            # roof
    d.polygon([p(50, 13), p(75, 29.5), p(50, 29.5)], fill=TOWER_DARK)       # its shaded side

    # The pennant on its pole
    d.line([p(50, 6), p(50, 15)], fill=TOWER_DARK, width=u(2.5))
    d.polygon([p(51, 6), p(63, 9), p(51, 12)], fill=TOWER_LIGHT)
    return im.resize((size, size), Image.LANCZOS)


def castle(size):
    im, d, p, u = canvas(size)

    # The curtain wall between the towers, its merlons, the arched gate
    d.rectangle([p(30, 48), p(70, 94)], fill=STONE_MID)
    for x in (33, 47, 61):
        d.rectangle([p(x, 41), p(x + 6, 48.5)], fill=STONE_MID)
    d.rectangle([p(41, 76), p(59, 94)], fill=STONE_DARK)
    d.pieslice([p(41, 67), p(59, 85)], 180, 360, fill=STONE_DARK)

    # The two towers: shaft, a wider parapet, three merlons, an arched window, a pennant
    for cx in (21, 79):
        d.rectangle([p(cx - 10, 36), p(cx + 10, 94)], fill=STONE)
        d.rectangle([p(cx - 13, 29), p(cx + 13, 37)], fill=STONE)
        d.rectangle([p(cx - 13, 36), p(cx + 13, 38)], fill=STONE_DARK)    # the parapet's ledge
        for x in (cx - 13, cx - 3, cx + 7):
            d.rectangle([p(x, 22), p(x + 6, 29.5)], fill=STONE)
        d.rectangle([p(cx - 2.5, 50), p(cx + 2.5, 62)], fill=STONE_DARK)
        d.pieslice([p(cx - 2.5, 47.5), p(cx + 2.5, 52.5)], 180, 360, fill=STONE_DARK)
        d.line([p(cx, 9), p(cx, 22)], fill=STONE_DARK, width=u(2.5))
        d.polygon([p(cx + 1, 9), p(cx + 12, 12), p(cx + 1, 15)], fill=STONE)
    return im.resize((size, size), Image.LANCZOS)


def windmill(size):
    im, d, p, u = canvas(size)
    hx, hy = 50, 42                                 # the hub

    # The body: a tapered tower, the cap, a window, the door
    d.polygon([p(36, 40), p(64, 40), p(74, 94), p(26, 94)], fill=MILL)
    d.pieslice([p(34, 25), p(66, 55)], 180, 360, fill=MILL_DARK)
    d.rectangle([p(33, 38.5), p(67, 42)], fill=MILL_DARK)
    d.rectangle([p(46.5, 59), p(53.5, 67)], fill=MILL_DARK)
    d.rectangle([p(43.5, 81), p(56.5, 94)], fill=MILL_DARK)
    d.pieslice([p(43.5, 74.5), p(56.5, 87.5)], 180, 360, fill=MILL_DARK)

    # Four sails in an X: a light blade on a stock, two cross bars
    r0, r1, half = 10, 39, 7.5
    for a in (45, 135, 225, 315):
        ux, uy = math.cos(math.radians(a)), math.sin(math.radians(a))
        vx, vy = -uy, ux
        at = lambda r, o: p(hx + ux * r + vx * o, hy + uy * r + vy * o)
        d.polygon([at(r0, -half), at(r1, -half), at(r1, half), at(r0, half)], fill=SAIL_EDGE)
        g = 1.6                                     # the outline, inside the blade
        d.polygon([at(r0 + g, -half + g), at(r1 - g, -half + g), at(r1 - g, half - g), at(r0 + g, half - g)], fill=SAIL)
        for r in (r0 + (r1 - r0) / 3, r0 + 2 * (r1 - r0) / 3):
            d.line([at(r, -half), at(r, half)], fill=SAIL_EDGE, width=u(2))
        d.line([at(0, 0), at(r1, 0)], fill=SAIL_EDGE, width=u(2.5))
    d.ellipse([p(hx - 4.5, hy - 4.5), p(hx + 4.5, hy + 4.5)], fill=MILL_DARK)
    return im.resize((size, size), Image.LANCZOS)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "rootfs/usr/share/x6100/field"
    os.makedirs(out, exist_ok=True)
    for name, draw in (("wwff", leaf), ("tota", tower), ("wca", castle), ("mota", windmill)):
        for size in (96, 64):
            draw(size).save(os.path.join(out, f"{name}_{size}.png"))
    print(f"wwff, tota, wca and mota icons in {out}")


if __name__ == "__main__":
    main()

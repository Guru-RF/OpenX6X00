#!/usr/bin/env python3
"""Field Ops home tiles, drawn with PIL at four times the size and scaled down, beside the SOTA, POTA,
WWBOTA, HOTA, Bands and Log icons the project was forked with:

  wwff_96.png, wwff_64.png   WWFF, a leaf in the tile's blue
  tota_96.png, tota_64.png   TOTA, a lookout tower in the tile's orange: lattice legs, railed platform, roof, pennant
  wca_96.png,  wca_64.png    WCA, a grey stone castle: two crenellated towers, a crenellated wall with a gate
  mota_96.png, mota_64.png   MOTA, a windmill in the tile's teal with four light lattice sails
  lh_96.png,   lh_64.png     LH, a lighthouse in the tile's coral red: red and white bands, a dark gallery, a lit
                             lantern with beams to both sides, on a rock
  tiles_96.png, tiles_64.png Tiles, a map of three by three tiles in the tile's lime, a pin on the bright one in the
                             middle
  llota_96.png, llota_64.png LLOTA, a lake in the tile's light blue with ripples, in a green shore under two hills,
                             reeds with their brown heads in front

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
BEACON = (248, 113, 113, 255)   # the LH tile's accent (theme role err)
BEACON_DARK = (185, 60, 60, 255)
BEACON_WHITE = (235, 240, 245, 255)
BEACON_WHITE_SHADE = (196, 205, 216, 255)
GALLERY = (71, 85, 105, 255)
GLASS = (255, 222, 100, 255)
BEAM = (255, 243, 180, 230)
ROCK = (120, 132, 150, 255)
ROCK_LIGHT = (165, 177, 194, 255)
LIME = (151, 201, 94, 255)      # the Tiles tile's accent (theme roles ok and warn mixed)
LIME_MID = (98, 140, 58, 255)
LIME_DARK = (70, 104, 40, 255)
PIN = (30, 41, 59, 255)
PIN_DOT = (235, 240, 245, 255)
LAKE = (143, 181, 247, 255)     # the LLOTA tile's accent (theme roles vfo and text mixed)
LAKE_DARK = (79, 139, 245, 255)
RIPPLE = (235, 240, 245, 255)
SHORE = (52, 160, 110, 255)
HILL = (100, 116, 139, 255)
HILL_FAR = (148, 163, 184, 255)
REED = (36, 110, 76, 255)
CATTAIL = (146, 96, 56, 255)


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


def lighthouse(size):
    im, d, p, u = canvas(size)
    top, foot = 36, 87                              # the shaft, from under the gallery to the rock
    hw = lambda y: 10 + 8 * (y - top) / (foot - top)    # its half width, 10 at the top to 18 at the foot

    # The beams, behind the lantern: a wedge to each side
    for sgn in (-1, 1):
        d.polygon([p(50 + sgn * 6, 20), p(50 + sgn * 44, 11), p(50 + sgn * 44, 31), p(50 + sgn * 6, 24)], fill=BEAM)

    # The rock, its lit top
    d.polygon([p(18, 94), p(25, 88), p(35, 84), p(65, 84), p(75, 87.5), p(82, 94)], fill=ROCK)
    d.polygon([p(25, 88), p(35, 84), p(65, 84), p(70, 85.5), p(42, 87.5)], fill=ROCK_LIGHT)

    # The shaft: five bands, red at the top and the foot, the right third in shade; the door
    bands = [top + i * (foot - top) / 5 for i in range(6)]
    for i, (y0, y1) in enumerate(zip(bands, bands[1:])):
        lit, shade = (BEACON, BEACON_DARK) if i % 2 == 0 else (BEACON_WHITE, BEACON_WHITE_SHADE)
        d.polygon([p(50 - hw(y0), y0), p(50 + hw(y0), y0), p(50 + hw(y1), y1), p(50 - hw(y1), y1)], fill=lit)
        d.polygon([p(50 + hw(y0) / 3, y0), p(50 + hw(y0), y0), p(50 + hw(y1), y1), p(50 + hw(y1) / 3, y1)], fill=shade)
    d.rectangle([p(46, 79), p(54, 87)], fill=GALLERY)
    d.pieslice([p(46, 75), p(54, 83)], 180, 360, fill=GALLERY)

    # The gallery: its deck, the railing over it
    d.rectangle([p(34, 32), p(66, 36.5)], fill=GALLERY)
    d.rectangle([p(36, 26), p(64, 28)], fill=GALLERY)                       # handrail
    for x in (37, 43.5, 50, 56.5, 63):
        d.rectangle([p(x - 1, 27), p(x + 1, 32)], fill=GALLERY)             # balusters

    # The lantern: the glass with a centre bar, the dome on its rim, the ball on top
    d.rectangle([p(42, 16), p(58, 28)], fill=GLASS)
    d.rectangle([p(49, 16), p(51, 28)], fill=GALLERY)
    d.rectangle([p(40, 14.5), p(60, 17)], fill=GALLERY)
    d.pieslice([p(41, 7), p(59, 23)], 180, 360, fill=GALLERY)
    d.ellipse([p(47.75, 4), p(52.25, 8.5)], fill=GALLERY)
    return im.resize((size, size), Image.LANCZOS)


def tiles(size):
    im, d, p, u = canvas(size)
    step, side = 31, 27                             # three tiles a row, 4 apart

    # The map: the tiles around in two shades, a checker; the one you are in bright
    for row in range(3):
        for col in range(3):
            x, y = 5 + col * step, 5 + row * step
            fill = LIME if (row, col) == (1, 1) else LIME_MID if (row + col) % 2 else LIME_DARK
            d.rounded_rectangle([p(x, y), p(x + side, y + side)], radius=u(4), fill=fill)

    # The pin on the middle tile: a round head drawn to a point at the tile's centre, a light dot in it
    cx, tip, r = 50, 56, 11
    hy = tip - 22
    d.ellipse([p(cx - r, hy - r), p(cx + r, hy + r)], fill=PIN)
    d.polygon([p(cx - r * 0.82, hy + r * 0.55), p(cx + r * 0.82, hy + r * 0.55), p(cx, tip)], fill=PIN)
    d.ellipse([p(cx - 4.5, hy - 4.5), p(cx + 4.5, hy + 4.5)], fill=PIN_DOT)
    return im.resize((size, size), Image.LANCZOS)


def lake(size):
    im, d, p, u = canvas(size)

    # Two hills behind, the far one lighter
    d.polygon([p(30, 54), p(64, 12), p(98, 54)], fill=HILL_FAR)
    d.polygon([p(2, 54), p(32, 22), p(62, 54)], fill=HILL)

    # The shore, the water in it: deeper blue along the far side, the tile's light blue towards you
    d.ellipse([p(1, 38), p(99, 98)], fill=SHORE)
    d.ellipse([p(7, 43), p(93, 93)], fill=LAKE_DARK)
    d.ellipse([p(7, 49), p(93, 93)], fill=LAKE)

    # Ripples: short light arcs on the water
    for x, y, w in ((20, 64, 22), (46, 73, 24), (26, 82, 16), (58, 58, 14)):
        d.arc([p(x, y - 3), p(x + w, y + 3)], 200, 340, fill=RIPPLE, width=u(2.4))

    # Reeds in front, on the right: thin stems, brown heads, two blades
    for x, top in ((74, 28), (81, 36), (88, 32)):
        d.line([p(x, 94), p(x, top + 9)], fill=REED, width=u(2.4))
        d.rounded_rectangle([p(x - 2.6, top), p(x + 2.6, top + 12)], radius=u(2.6), fill=CATTAIL)
        d.line([p(x, top), p(x, top - 5)], fill=REED, width=u(1.6))
    d.polygon([p(70, 94), p(64, 56), p(73.5, 90)], fill=REED)
    d.polygon([p(92, 94), p(97, 62), p(89, 90)], fill=REED)
    return im.resize((size, size), Image.LANCZOS)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "rootfs/usr/share/x6100/field"
    os.makedirs(out, exist_ok=True)
    for name, draw in (("wwff", leaf), ("tota", tower), ("wca", castle), ("mota", windmill), ("lh", lighthouse),
                       ("tiles", tiles), ("llota", lake)):
        for size in (96, 64):
            draw(size).save(os.path.join(out, f"{name}_{size}.png"))
    print(f"wwff, tota, wca, mota, lh, tiles and llota icons in {out}")


if __name__ == "__main__":
    main()

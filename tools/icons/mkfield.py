#!/usr/bin/env python3
"""Field Ops home tiles: the WWFF icon (wwff_96.png, wwff_64.png), a leaf in the tile's blue, drawn with
PIL at four times the size and scaled down, beside the SOTA, POTA, WWBOTA, HOTA, Bands and Log icons
the project was forked with.

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


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "rootfs/usr/share/x6100/field"
    os.makedirs(out, exist_ok=True)
    for size in (96, 64):
        leaf(size).save(os.path.join(out, f"wwff_{size}.png"))
    print(f"wwff icons in {out}")


if __name__ == "__main__":
    main()

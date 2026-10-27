#!/usr/bin/env python3
"""Are two builds' screens the same? A1 and A2 are two runs of the old build, B one of the new,
each a folder of shots with the same names (host_shots.py). What moves by itself (the spectrum and
waterfall noise, the clock, the S-meter) differs between A1 and A2: those pixels, 3 px around them,
and the clock and S-meter boxes are left out; everything else of A1 and B must be equal.

  python3 tools/screenshots/compare.py A1 A2 B

A shot that differs gets B/<name>_diff.png (the differences in magenta over a dimmed A1); the exit
status is then 1.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import os
import sys

from PIL import Image, ImageChops, ImageFilter

STATIC_MASK = [(630, 2, 797, 63), (365, 2, 626, 63)]   # clock, S-meter (the standard layout)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    a1d, a2d, bd = sys.argv[1:]
    status = 0
    # every shot of the three folders: one missing from any of them fails, and so do no shots at all
    names = sorted({n for d in (a1d, a2d, bd) for n in os.listdir(d) if n.endswith(".png") and not n.endswith("_diff.png")})
    if not names:
        print("no shots to compare")
        sys.exit(1)
    for name in names:
        try:
            a1, a2, b = (Image.open(os.path.join(d, name)).convert("RGB") for d in (a1d, a2d, bd))
        except FileNotFoundError as e:
            print(f"{name}: {e}")
            status = 1
            continue
        moving = ImageChops.difference(a1, a2).convert("L").point(lambda v: 255 if v else 0)
        moving = moving.filter(ImageFilter.MaxFilter(7))   # 3 px around
        for box in STATIC_MASK:
            moving.paste(255, box)
        diff = ImageChops.difference(a1, b).convert("L").point(lambda v: 255 if v else 0)
        diff = ImageChops.subtract(diff, moving)
        n = sum(1 for v in diff.getdata() if v)
        masked = sum(1 for v in moving.getdata() if v)
        if n:
            status = 1
            dim = Image.eval(a1, lambda v: v // 3)
            dim.paste((255, 0, 255), mask=diff)
            dim.save(os.path.join(bd, name[:-4] + "_diff.png"))
            print(f"{name}: {n} pixels differ (box {diff.getbbox()}), {masked} left out as moving")
        else:
            print(f"{name}: same ({masked} pixels left out as moving)")
    sys.exit(status)


if __name__ == "__main__":
    main()

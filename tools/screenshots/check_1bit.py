#!/usr/bin/env python3
"""The 1bit theme's two-colour test: every pixel of a screenshot must be exactly the ink or exactly
the paper, and both must be there (one colour only is a blank or broken shot).

  python3 tools/screenshots/check_1bit.py [--ink 000000] [--paper efece2] shot.png|shot.raw ...
  python3 tools/screenshots/check_1bit.py --self-test

For each file: the number of other pixels, their colours (the ten most common), where they are
(16 px cells, merged into boxes) and <name>_offenders.png, the offenders in magenta over a dimmed
copy. A .raw is a GUI dump (raw2png.py). Exit status 1 when any file fails.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import argparse
import os
import sys
import tempfile
from collections import Counter

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from raw2png import raw_image  # noqa: E402

CELL = 16


def boxes(cells):
    """The 16 px cells with offenders merged into boxes (touching cells join)."""
    cells, out = set(cells), []
    while cells:
        stack, group = [cells.pop()], []
        while stack:
            c = stack.pop()
            group.append(c)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    n = (c[0] + dx, c[1] + dy)
                    if n in cells:
                        cells.remove(n)
                        stack.append(n)
        xs, ys = [g[0] for g in group], [g[1] for g in group]
        out.append((min(xs) * CELL, min(ys) * CELL, (max(xs) + 1) * CELL - 1, (max(ys) + 1) * CELL - 1))
    return sorted(out)


def check(path, ink, paper, write=True):
    if path.endswith(".raw"):
        with open(path, "rb") as f:
            im = raw_image(f.read())
        if im is None:
            print(f"{path}: not a screen dump")
            return False
    else:
        im = Image.open(path).convert("RGB")
    px = im.load()
    w, h = im.size
    bad, cells, seen = Counter(), set(), set()
    for y in range(h):
        for x in range(w):
            c = px[x, y]
            if c == ink or c == paper:
                seen.add(c)
                continue
            bad[c] += 1
            cells.add((x // CELL, y // CELL))
    n = sum(bad.values())
    if not n and len(seen) == 2:
        print(f"{path}: two colours")
        return True
    if not n:
        print(f"{path}: only {'#%02x%02x%02x' % next(iter(seen)) if seen else 'nothing'}: a blank shot?")
        return False
    print(f"{path}: {n} pixels neither ink nor paper")
    for c, k in bad.most_common(10):
        print(f"    #{c[0]:02x}{c[1]:02x}{c[2]:02x}  {k}")
    for b in boxes(cells)[:20]:
        print(f"    in {b}")
    if write:
        out = Image.eval(im, lambda v: v // 2)
        o = out.load()
        for y in range(h):
            for x in range(w):
                if px[x, y] != ink and px[x, y] != paper:
                    o[x, y] = (255, 0, 255)
        out.save(os.path.splitext(path)[0] + "_offenders.png")
    return False


def rgb(s):
    s = s.lstrip("#")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def self_test(ink, paper):
    with tempfile.TemporaryDirectory() as d:
        im = Image.new("RGB", (4, 4), paper)
        im.putpixel((0, 0), ink)
        p = os.path.join(d, "good.png")
        im.save(p)
        good = check(p, ink, paper, False)
        im.putpixel((2, 2), (128, 128, 128))
        p = os.path.join(d, "bad.png")
        im.save(p)
        bad = check(p, ink, paper, False)
        blank = os.path.join(d, "blank.png")
        Image.new("RGB", (4, 4), paper).save(blank)
        one = check(blank, ink, paper, False)
    ok = good and not bad and not one
    print("self-test:", "passed" if ok else "FAILED")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ink", default="000000")
    ap.add_argument("--paper", default="efece2")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    ink, paper = rgb(a.ink), rgb(a.paper)
    if a.self_test:
        sys.exit(0 if self_test(ink, paper) else 1)
    ok = True
    for f in a.files:
        if f.endswith("_offenders.png"):
            continue
        ok = check(f, ink, paper) and ok
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""A screen dump (/tmp/screen.raw of the GUI's SIGUSR1: the radio's DRM composite or the PC
build's window) as a PNG. The dump is the portrait panel, 480 x 800, XRGB8888 little-endian;
the PNG is landscape, or the panel turned 180 for a portrait screen (Walkie).

  python3 tools/screenshots/raw2png.py screen.raw out.png [--portrait]

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import sys

from PIL import Image

PANEL = (480, 800)


def raw_image(data, portrait=False):
    """The dump's bytes as a landscape (or turned portrait) RGB image; None when the size is wrong."""
    if len(data) != PANEL[0] * PANEL[1] * 4:
        return None
    im = Image.frombuffer("RGBA", PANEL, data, "raw", "BGRA", 0, 1).convert("RGB")
    return im.rotate(180 if portrait else -90, expand=True)


def raw_to_png(raw, png, portrait=False):
    with open(raw, "rb") as f:
        im = raw_image(f.read(), portrait)
    if im is None:
        return False
    im.save(png)
    return True


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if not raw_to_png(sys.argv[1], sys.argv[2], "--portrait" in sys.argv[3:]):
        sys.exit(f"{sys.argv[1]}: not a 480 x 800 x 4 dump")

#!/usr/bin/env python3
"""Copies the screenshots taken on the radio into docs/screenshots, through the web configurator
(the Gallery's "shots": the PNG files on the DATA partition).

  python3 tools/screenshots/pull.py 192.168.1.20 [--all]

New files only, unless --all. Needs nothing but python3.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import json
import os
import sys
import urllib.request

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    host = sys.argv[1]
    everything = "--all" in sys.argv
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "docs", "screenshots")
    os.makedirs(out, exist_ok=True)
    with urllib.request.urlopen(f"http://{host}/api/gallery?kind=shots", timeout=20) as r:
        items = json.load(r)
    got = 0
    for it in items:
        name = it["name"]
        dest = os.path.join(out, name)
        if os.path.exists(dest) and not everything:
            continue
        with urllib.request.urlopen(f"http://{host}/gallery/file/shots/{urllib.request.quote(name)}", timeout=60) as r, open(dest, "wb") as f:
            f.write(r.read())
        got += 1
        print(name)
    print(f"{got} new of {len(items)} on the radio -> {out}")

if __name__ == "__main__":
    main()

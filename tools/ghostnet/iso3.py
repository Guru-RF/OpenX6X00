#!/usr/bin/env python3
"""The ISO 3166-1 alpha-2 and alpha-3 codes of every country, for the GhostNet regional group (@GN + the alpha-3 +
the department): src/ghostnet_iso3.inc. Needs pycountry (pip install pycountry) only to make it again.

SPDX-License-Identifier: LGPL-2.1-or-later
After the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico)."""
import os

import pycountry

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "src", "ghostnet_iso3.inc")
rows = sorted((c.alpha_2, c.alpha_3) for c in pycountry.countries)
with open(OUT, "w") as f:
    f.write("/* Made by tools/ghostnet/iso3.py, not edited by hand: {ISO 3166-1 alpha-2, alpha-3} of every country, "
            "for the\n * GhostNet regional group (@GN + the alpha-3 + the department, src/ghostnet.c) */\n")
    for i in range(0, len(rows), 6):
        f.write("    " + " ".join('{"%s", "%s"},' % r for r in rows[i:i + 6]) + "\n")
print(len(rows), "->", OUT)

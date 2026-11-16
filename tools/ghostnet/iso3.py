#!/usr/bin/env python3
"""The ISO 3166-1 alpha-2 and alpha-3 codes of every country, for the GhostNet regional group (@GN + the alpha-3 +
the department): src/ghostnet_iso3.inc for the GUI and webserver/.../iso3_web.py for the web page GhostNet, the same
rows (ghostnet_web_test.py checks that they agree). Needs pycountry (pip install pycountry) only to make them again.

SPDX-License-Identifier: LGPL-2.1-or-later
After the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico)."""
import os

import pycountry

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_C = os.path.join(ROOT, "src", "ghostnet_iso3.inc")
OUT_PY = os.path.join(ROOT, "webserver", "src", "x6100_webserver", "iso3_web.py")
rows = sorted((c.alpha_2, c.alpha_3) for c in pycountry.countries)


def write_c(f):
    f.write("/* Made by tools/ghostnet/iso3.py, not edited by hand: {ISO 3166-1 alpha-2, alpha-3} of every country, "
            "for the\n * GhostNet regional group (@GN + the alpha-3 + the department, src/ghostnet.c) */\n")
    for i in range(0, len(rows), 6):
        f.write("    " + " ".join('{"%s", "%s"},' % r for r in rows[i:i + 6]) + "\n")


def write_py(f):
    f.write('"""Made by tools/ghostnet/iso3.py, not edited by hand: ISO 3166-1 alpha-2 -> alpha-3 of every country, '
            'for the\nGhostNet regional group (@GN + the alpha-3 + the department, ghostnet_web.py)."""\nISO3 = {\n')
    for i in range(0, len(rows), 8):
        f.write("    " + " ".join("'%s': '%s'," % r for r in rows[i:i + 8]) + "\n")
    f.write("}\n")


if __name__ == "__main__":
    for path, write in ((OUT_C, write_c), (OUT_PY, write_py)):
        with open(path, "w") as f:
            write(f)
        print(len(rows), "->", path)

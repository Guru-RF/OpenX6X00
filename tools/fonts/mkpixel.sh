#!/bin/sh
# The 1bit theme's pixel family (src/fonts/pixel_*.c) from the BDF files of Terminus Font 4.49.1
# (terminus-font-4.49.1.tar.gz, sha256 d961c1b781627bf417f9b340693d64fc219e0113ad3a3af1a3424c7aa373ef79,
# unpacked into a folder of its own):
#
#   sh tools/fonts/mkpixel.sh <folder with ter-u*.bdf> [--check]
#
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
set -e
T=$1; shift
[ -f "$T/ter-u20n.bdf" ] || { echo "usage: mkpixel.sh <folder with ter-u*.bdf> [--check]" >&2; exit 2; }
cd "$(dirname "$0")/../.."
one() {   # name bdf [converter options]
    n=$1; f=$2; shift 2
    python3 -I tools/fonts/bdf2lvgl.py "$T/$f" "$n" "src/fonts/$n.c" "$@" --licence \
"$n: 1 bpp, from $f of Terminus Font 4.49.1, Copyright (C) 2020 Dimitar Toshkov
Zhekov, SIL Open Font License 1.1 (src/fonts/LICENSE-Terminus-OFL-1.1.txt). A Modified Version
under the OFL: not \"Terminus Font\"."
}
one pixel_12 ter-u12n.bdf "$@"
one pixel_14 ter-u14n.bdf "$@"
one pixel_16 ter-u16n.bdf "$@"
one pixel_18 ter-u18n.bdf "$@"
one pixel_20 ter-u20n.bdf "$@"
one pixel_22 ter-u22n.bdf "$@"
one pixel_24 ter-u24n.bdf "$@"
one pixel_28 ter-u28n.bdf "$@"
one pixel_32 ter-u32n.bdf "$@"
one pixel_44 ter-u22n.bdf --scale 2 "$@"
one pixel_freq_40 ter-u20b.bdf --scale 2 --range 0x20-0x7e "$@"

#!/bin/sh
# The letters beyond ASCII: the capitals the CW decoder and the CW Keyboard show (#265), Å Ä Ö Ü É Ñ, their small
# letters typed on a keyboard with such a layout into the text boxes (#269), and the rest of Latin-1 for the GUI's
# languages (below), as fonts of their own
# (src/fonts/latin1_*.c): DejaVu Sans at the pixel size
# of each dejavu_* (read from its header), with lv_font_conv 1.5.3 as that family was made, from LVGL's
# copy of the font:
#
#   sh tools/fonts/mklatin.sh
#
# LV_FONT_CONV: the converter (default npx's lv_font_conv@1.5.3); DEJAVU: DejaVuSans.ttf, when the lvgl
# submodule is not checked out here.
#
# The decoder and the CW Keyboard show the capitals, from the Morse table (src/cw_decoder/morse_code.c), the text
# boxes what was typed. Since the GUI speaks other languages (src/i18n.h, po/) the whole of Latin-1 is here
# (U+00A0..U+00FF: Spanish, French, Dutch, German, Italian, Portuguese, the Nordic languages), with the French
# Œ œ, the dashes, the quotes of each language, the ellipsis and the euro sign: about 210 kB for the seventeen
# sizes. A script beyond it (Chinese, Cyrillic) comes from the TrueType font its language file names
# (X-OpenX6X00-Font), drawn as needed (font_select.c).
# The GUI's fonts are ASCII but the pixel family's: font_select.c hangs one of these behind each, and a letter
# a font lacks is drawn from here. Not added to the fonts themselves: lv_font_conv gives a font the height of
# its tallest glyph, the accent over a capital takes dejavu_24's line from 21 to 24 pixels, and every text the
# GUI lays out would move. The same lv_font_conv and font make dejavu_*.c as they are, glyph for glyph.
#
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
set -e
cd "$(dirname "$0")/../.."
CONV=${LV_FONT_CONV:-npx -y lv_font_conv@1.5.3}
F=${DEJAVU:-lvgl/scripts/built_in_font/DejaVuSans.ttf}
[ -f "$F" ] || { echo "mklatin.sh: no $F (git submodule update --init lvgl, or DEJAVU=<DejaVuSans.ttf>)" >&2; exit 2; }
OUT=$PWD/src/fonts
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
RANGE=0xA0-0xFF,0x152-0x153,0x2013-0x2014,0x2018-0x201A,0x201C-0x201E,0x2026,0x20AC
cp "$F" "$W/DejaVuSans.ttf"
cd "$W"     # the headers name the font and the file alone, as dejavu_*.c's do
for n in 14 16 18 20 22 24 26 28 30 32 34 36 38 40 42 44 60; do
    px=$(sed -n 's/^ \* Size: \([0-9]*\) px$/\1/p' "$OUT/dejavu_$n.c")
    [ -n "$px" ] || { echo "mklatin.sh: no size in dejavu_$n.c" >&2; exit 2; }
    $CONV --bpp 4 --size "$px" --no-compress --font DejaVuSans.ttf --range "$RANGE" \
        --format lvgl -o "latin1_$n.c"
    mv "latin1_$n.c" "$OUT/"
done

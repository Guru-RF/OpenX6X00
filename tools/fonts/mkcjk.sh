#!/bin/sh
# The Chinese font the zh_CN language file names (X-OpenX6X00-Font, src/i18n.h): Noto Sans CJK SC Regular (SIL Open
# Font License 1.1), cut down to GB2312's 7445 characters (its 6763 ideographs), the CJK punctuation, the full-width
# forms and every character po/zh_CN.po uses, outlines as they are (CFF) without hinting or layout tables: about
# 1.5 MB in rootfs/usr/share/x6100/fonts, drawn as needed by LVGL's TrueType reader (src/fonts/font_select.c).
#
#   sh tools/fonts/mkcjk.sh
#
# NOTO_CJK: NotoSansCJK-Regular.ttc (default: a Linux PC's, /usr/share/fonts/noto-cjk) or NotoSansCJKsc-Regular.otf
# from https://github.com/notofonts/noto-cjk. pyftsubset (fontTools) does the cutting. Run it again when the Chinese
# translation takes a character beyond GB2312.
#
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
set -e
cd "$(dirname "$0")/../.."
F=${NOTO_CJK:-/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc}
[ -f "$F" ] || { echo "mkcjk.sh: no $F (NOTO_CJK=<NotoSansCJK-Regular.ttc or NotoSansCJKsc-Regular.otf>)" >&2; exit 2; }
command -v pyftsubset > /dev/null || { echo "mkcjk.sh: no pyftsubset (pip install fonttools)" >&2; exit 2; }
OUT=$PWD/rootfs/usr/share/x6100/fonts
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# the characters, and in a collection the number of the Simplified Chinese font
python3 -I - "$F" "$PWD/po/zh_CN.po" "$W" <<'EOF'
import sys
from fontTools.ttLib import TTCollection
font, po, w = sys.argv[1:4]
chars = set()
for b1 in range(0xA1, 0xF8):
    for b2 in range(0xA1, 0xFF):
        try:
            chars.add(bytes([b1, b2]).decode('gb2312'))
        except UnicodeDecodeError:
            pass
for r in ((0x2010, 0x2030), (0x3000, 0x3040), (0xFF01, 0xFF5F)):
    chars.update(chr(c) for c in range(*r))
try:
    chars.update(c for c in open(po, encoding='utf-8').read() if ord(c) > 0x7F)
except FileNotFoundError:
    pass
open(w + '/chars.txt', 'w', encoding='utf-8').write(''.join(sorted(chars)))
n = 0
if font.lower().endswith('.ttc'):
    names = [f['name'].getDebugName(4) for f in TTCollection(font).fonts]
    n = names.index('Noto Sans CJK SC')
open(w + '/number', 'w').write(str(n))
EOF
pyftsubset "$F" --font-number="$(cat "$W/number")" --text-file="$W/chars.txt" --layout-features='' --no-hinting \
    --notdef-outline --name-IDs='*' --output-file="$OUT/NotoSansCJKsc-GB2312.otf"
# who made it and its licence beside it; the licence's text is OFL.txt there
python3 -I - "$OUT/NotoSansCJKsc-GB2312.otf" > "$OUT/NotoSansCJKsc-GB2312.txt" <<'EOF'
import sys
from fontTools.ttLib import TTFont
name = TTFont(sys.argv[1])['name']
print(name.getDebugName(0))
print(name.getDebugName(5))
print()
print("NotoSansCJKsc-GB2312.otf is Noto Sans CJK SC Regular cut down to the characters OpenX6X00's Chinese uses")
print("(tools/fonts/mkcjk.sh). It is licensed under the SIL Open Font License, Version 1.1: OFL.txt here.")
EOF
ls -l "$OUT"

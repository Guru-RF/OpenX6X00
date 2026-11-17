"""The glyphs of the callsign on the boot logo (bootanim/boot_call.c, #292), from Inter Bold as the logo's name.
After the CX8ANM edition's (condoruy): the same font and size, each glyph's rows cut to what any glyph inks.
Usage: python3 -I tools/logo/mkcallfont.py assets/fonts bootanim/callfont.h

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

fontdir, out = sys.argv[1], sys.argv[2]
CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/-"   # what a callsign has (station.c), a CB one too
SIZE = 44
SPACING = 3
font = ImageFont.truetype(os.path.join(fontdir, "Inter-Bold.ttf"), SIZE)
asc, desc = font.getmetrics()
H = asc + desc
glyphs = []
for c in CHARS:
    w = int(font.getlength(c)) + 2
    im = Image.new("L", (w, H), 0)
    ImageDraw.Draw(im).text((1, 0), c, font=font, fill=255)
    glyphs.append((c, w, im))
# the rows any glyph inks, so every glyph keeps its place on the baseline
top = min(im.getbbox()[1] for _, _, im in glyphs)
bottom = max(im.getbbox()[3] for _, _, im in glyphs)
HEX = ".123456789abcdef"   # 16 levels, "." none: the glyph shows as a picture in the source
with open(out, "w") as f:
    f.write("/* Made by tools/logo/mkcallfont.py from Inter Bold %dpx: do not edit. The callsign on the boot logo\n"
            " * (boot_call.c): each glyph %d rows of its width, a pixel's ink '.' none, '1' to 'f' up to full */\n"
            % (SIZE, bottom - top))
    f.write("#define CALLFONT_H %d\n#define CALLFONT_SPACING %d\n" % (bottom - top, SPACING))
    for i, (c, w, im) in enumerate(glyphs):
        px = [im.getpixel((x, y)) for y in range(top, bottom) for x in range(w)]
        rows = ['"' + "".join(HEX[(p * 15 + 127) // 255] for p in px[y * w:(y + 1) * w]) + '"'
                for y in range(bottom - top)]
        f.write("static const char callfont_%d[] =   /* %s */\n    %s;\n" % (i, c, "\n    ".join(rows)))
    f.write("static const struct {\n    char        c;\n    uint8_t     w;\n    const char *px;\n} callfont[] = {\n")
    for i, (c, w, im) in enumerate(glyphs):
        f.write("    {'%s', %d, callfont_%d},\n" % (c, w, i))
    f.write("};\n")
print(len(glyphs), "glyphs,", bottom - top, "rows ->", out)

#!/usr/bin/env python3
"""A BDF bitmap font as an LVGL 8 font (lv_font_fmt_txt) at 1 bpp: every pixel ink or not, no
anti-aliasing. --scale n draws each pixel as an n x n block, so a big size keeps the square,
crisp pixels of a designed bitmap size. The C file is laid out like lv_font_conv's.

  python3 tools/fonts/bdf2lvgl.py FONT.bdf NAME OUT.c [--scale N] [--range 0x20-0x7e,0xa0-0xff]
                                  [--licence TEXT] [--check]

--licence puts TEXT (a few lines, "\\n" between them) at the top of the file, after the SPDX line
of --spdx (default OFL-1.1). --check also writes OUT.png: "Hello 14.074.000 °" drawn from the
generated glyph data, to look at before committing. Pure Python 3, no other module.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import argparse
import os
import struct
import sys
import zlib


def parse_bdf(path):
    """{codepoint: (dwidth, w, h, xoff, yoff, rows)}, ascent, descent. rows: lists of 0/1, top first."""
    glyphs, ascent, descent = {}, None, None
    with open(path, encoding="latin-1") as f:
        lines = iter(f.read().splitlines())
    for line in lines:
        w = line.split()
        if not w:
            continue
        if w[0] == "FONT_ASCENT":
            ascent = int(w[1])
        elif w[0] == "FONT_DESCENT":
            descent = int(w[1])
        elif w[0] == "STARTCHAR":
            enc, dw, bbx, rows = None, None, None, []
            for line in lines:
                w = line.split()
                if not w:
                    continue
                if w[0] == "ENCODING":
                    enc = int(w[1])
                elif w[0] == "DWIDTH":
                    dw = int(w[1])
                elif w[0] == "BBX":
                    bbx = tuple(int(v) for v in w[1:5])
                elif w[0] == "BITMAP":
                    for _ in range(bbx[1]):
                        hexrow = next(lines).strip()
                        bits = bin(int(hexrow, 16))[2:].zfill(len(hexrow) * 4)
                        rows.append([int(b) for b in bits[:bbx[0]]])
                elif w[0] == "ENDCHAR":
                    break
            if enc is not None and enc >= 0 and bbx:
                glyphs[enc] = (dw if dw is not None else bbx[0], bbx[0], bbx[1], bbx[2], bbx[3], rows)
    if ascent is None or descent is None:
        sys.exit(f"{path}: no FONT_ASCENT / FONT_DESCENT")
    return glyphs, ascent, descent


def crop(g):
    """The glyph cut to its ink: (adv, box_w, box_h, ofs_x, ofs_y, rows)."""
    dw, w, h, xoff, yoff, rows = g
    ys = [y for y in range(h) if any(rows[y])]
    xs = [x for x in range(w) if any(r[x] for r in rows)]
    if not ys or not xs:
        return dw, 0, 0, 0, 0, []
    top, bottom, left, right = ys[0], ys[-1], xs[0], xs[-1]
    rows = [r[left:right + 1] for r in rows[top:bottom + 1]]
    return dw, right - left + 1, bottom - top + 1, xoff + left, yoff + (h - 1 - bottom), rows


def scale(g, s):
    adv, w, h, ox, oy, rows = g
    rows = [[b for b in r for _ in range(s)] for r in rows for _ in range(s)]
    return adv * s, w * s, h * s, ox * s, oy * s, rows


def pack(rows):
    """LVGL's plain 1 bpp layout: the rows back to back as one bit stream, MSB first."""
    bits = [b for r in rows for b in r]
    out = bytearray()
    for i in range(0, len(bits), 8):
        chunk = bits[i:i + 8] + [0] * (8 - len(bits[i:i + 8]))
        out.append(int("".join(map(str, chunk)), 2))
    return bytes(out)


def ranges_of(text):
    out = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        out.append((int(a, 0), int(b or a, 0)))
    return out


def png(path, w, h, pixels):
    """A greyscale PNG; pixels: rows of 0..255."""
    raw = b"".join(b"\0" + bytes(r) for r in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def check(path, glyphs, line_height, base_line):
    """Draws a test line from the packed data, as lv_draw_sw_letter() places the glyphs."""
    text = "Hello 14.074.000 °"
    pad = 4
    width = sum(glyphs.get(ord(c), (0,))[0] for c in text) + 2 * pad
    img = [[255] * width for _ in range(line_height + 2 * pad)]
    x = pad
    for c in text:
        g = glyphs.get(ord(c))
        if not g:
            continue
        adv, bw, bh, ox, oy, data = g
        gy = pad + (line_height - base_line) - bh - oy
        for i in range(bw * bh):
            if data[i >> 3] >> (7 - (i & 7)) & 1:
                yy, xx = gy + i // bw, x + ox + i % bw
                if 0 <= yy < len(img) and 0 <= xx < width:
                    img[yy][xx] = 0
        x += adv
    png(path, width, len(img), img)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("bdf")
    ap.add_argument("name")
    ap.add_argument("out")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--range", default="0x20-0x7e,0xa0-0xff")
    ap.add_argument("--spdx", default="OFL-1.1")
    ap.add_argument("--licence", default="")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.scale < 1:
        sys.exit("--scale is a whole number from 1")

    src, ascent, descent = parse_bdf(a.bdf)
    ranges = ranges_of(a.range)
    cell = max(g[0] for g in src.values())

    glyphs, bitmap, dsc = {}, bytearray(), []
    for lo, hi in ranges:
        for cp in range(lo, hi + 1):
            g = crop(src[cp]) if cp in src else (cell, 0, 0, 0, 0, [])
            g = scale(g, a.scale)
            adv, w, h, ox, oy, rows = g
            data = pack(rows)
            if adv * 16 > 4095 or w > 255 or h > 255 or not -128 <= ox <= 127 or not -128 <= oy <= 127:
                sys.exit(f"U+{cp:04X}: does not fit LVGL's glyph description at scale {a.scale}")
            dsc.append((len(bitmap), adv * 16, w, h, ox, oy, cp))
            glyphs[cp] = (adv, w, h, ox, oy, data)
            bitmap += data
    if len(bitmap) >= 1 << 20:
        sys.exit("more than 1 MB of bitmaps")

    line_height, base_line = (ascent + descent) * a.scale, descent * a.scale
    args = " ".join([os.path.basename(a.bdf), a.name, os.path.basename(a.out)]
                    + (["--scale", str(a.scale)] if a.scale != 1 else [])
                    + (["--range", a.range] if a.range != "0x20-0x7e,0xa0-0xff" else []))
    o = []
    o.append(f"/* SPDX-License-Identifier: {a.spdx}")
    for line in a.licence.replace("\\n", "\n").splitlines():
        o.append(f" * {line}".rstrip())
    o.append(f" * Converted by tools/fonts/bdf2lvgl.py {args}")
    o.append(" */")
    o.append("")
    o.append("/*******************************************************************************")
    o.append(f" * Size: {line_height} px")
    o.append(" * Bpp: 1")
    o.append(f" * Scale: {a.scale}")
    o.append(" ******************************************************************************/")
    o.append("")
    o.append("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    o.append('#include "lvgl.h"')
    o.append("#else")
    o.append('#include "lvgl/lvgl.h"')
    o.append("#endif")
    o.append("")
    o.append("/*Store the image of the glyphs*/")
    o.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for (idx, _, w, h, _, _, cp) in dsc:
        g = glyphs[cp]
        ch = chr(cp)
        label = "\\\\" if ch == "\\" else ch if 0x20 < cp < 0x7f else ""
        o.append(f"    /* U+{cp:04X} \"{label}\" */" if label else f"    /* U+{cp:04X} */")
        data = g[5]
        for i in range(0, len(data), 12):
            o.append("    " + " ".join(f"0x{b:02x}," for b in data[i:i + 12]))
    if not bitmap:
        o.append("    0x00")
    o.append("};")
    o.append("")
    o.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    o.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
    for i, (idx, adv16, w, h, ox, oy, cp) in enumerate(dsc):
        sep = "," if i + 1 < len(dsc) else ""
        o.append(f"    {{.bitmap_index = {idx}, .adv_w = {adv16}, .box_w = {w}, .box_h = {h}, "
                 f".ofs_x = {ox}, .ofs_y = {oy}}}{sep}")
    o.append("};")
    o.append("")
    o.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    gid = 1
    for i, (lo, hi) in enumerate(ranges):
        sep = "," if i + 1 < len(ranges) else ""
        o.append(f"    {{.range_start = {lo}, .range_length = {hi - lo + 1}, .glyph_id_start = {gid}, "
                 f".unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
                 f".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}}{sep}")
        gid += hi - lo + 1
    o.append("};")
    o.append("")
    o.append("static lv_font_fmt_txt_glyph_cache_t cache;")
    o.append("")
    o.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    o.append("    .glyph_bitmap = glyph_bitmap,")
    o.append("    .glyph_dsc = glyph_dsc,")
    o.append("    .cmaps = cmaps,")
    o.append("    .kern_dsc = NULL,")
    o.append("    .kern_scale = 0,")
    o.append(f"    .cmap_num = {len(ranges)},")
    o.append("    .bpp = 1,")
    o.append("    .kern_classes = 0,")
    o.append("    .bitmap_format = 0,")
    o.append("    .cache = &cache")
    o.append("};")
    o.append("")
    o.append(f"const lv_font_t {a.name} = {{")
    o.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    o.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    o.append(f"    .line_height = {line_height},")
    o.append(f"    .base_line = {base_line},")
    o.append("    .subpx = LV_FONT_SUBPX_NONE,")
    o.append(f"    .underline_position = {-a.scale},")
    o.append(f"    .underline_thickness = {a.scale},")
    o.append("    .dsc = &font_dsc,")
    o.append("    .fallback = NULL,")
    o.append("    .user_data = NULL,")
    o.append("};")
    with open(a.out, "w") as f:
        f.write("\n".join(o) + "\n")
    if a.check:
        check(os.path.splitext(a.out)[0] + ".png", glyphs, line_height, base_line)
    print(f"{a.out}: {len(dsc)} glyphs, {len(bitmap)} bytes of bitmaps, line {line_height} / base {base_line}")


if __name__ == "__main__":
    main()

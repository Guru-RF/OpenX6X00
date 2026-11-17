/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The callsign on the boot logo: see boot_call.h. The drawing after the CX8ANM edition's draw_call() (condoruy)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "boot_call.h"

#include "callfont.h"

#include <stddef.h>

static const uint8_t gold[3] = {229, 168, 35};      /* the ring's and X6X00's (tools/logo/mklogo.py) */

/* The glyph of c in callfont, small letters as capitals; -1 none */
static int glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    for (size_t i = 0; i < sizeof(callfont) / sizeof(callfont[0]); i++)
        if (callfont[i].c == c) return (int)i;
    return -1;
}

/* A pixel's ink, 0 to 255: '.' none, '1' to 'f' (mkcallfont.py) */
static int ink(char p) {
    return p == '.' ? 0 : (p <= '9' ? p - '0' : p - 'a' + 10) * 17;
}

bool boot_call_draw(uint8_t *logo, int w, int h, const char *call) {
    int width = 0;

    if (!call) return false;
    for (const char *c = call; *c; c++) {
        int g = glyph(*c);

        if (g >= 0) width += callfont[g].w + CALLFONT_SPACING;
    }
    if (width == 0 || width > w - 40 || h < BOOT_CALL_BOTTOM) return false;
    width -= CALLFONT_SPACING;

    int x = (w - width) / 2, y0 = (BOOT_CALL_BOTTOM - CALLFONT_H) / 2;

    for (const char *c = call; *c; c++) {
        int g = glyph(*c);

        if (g < 0) continue;
        for (int y = 0; y < CALLFONT_H; y++) {
            for (int i = 0; i < callfont[g].w; i++) {
                int      a = ink(callfont[g].px[y * callfont[g].w + i]);
                uint8_t *p = logo + ((y0 + y) * w + x + i) * 3;

                if (!a) continue;
                for (int k = 0; k < 3; k++) p[k] = (uint8_t)((p[k] * (255 - a) + gold[k] * a) / 255);
            }
        }
        x += callfont[g].w + CALLFONT_SPACING;
    }
    return true;
}

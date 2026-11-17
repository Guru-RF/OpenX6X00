/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the panel stripes fix (#239): the display engine's gamma table
 *
 *  Some X6100 panels (a newer board, the same panel type) show vertical coloured stripes wherever a colour channel
 *  is 8 to 31 out of 255: the dark bars and buttons, the see-through panels over a dark waterfall. The framebuffer is
 *  clean; it happens on the way to the panel, and follows the values that reach it. The processor's panel controller
 *  (its TCON0; both radios have the same processor) sends every pixel through a 256-entry gamma table on its way out,
 *  per channel, after the blending of both planes, at no cost to the processor. Table A sends 8..31 out as 32 and the
 *  rest as it is: tested on such a radio, it clears the stripes, the darkest shades a little lighter. Values of 32 to
 *  43 are not safe on that panel either (green stripes in a test), so 32 is the step and no further.
 *
 *  The registers, from the processor's manual: in the global control (0x000) bit 31 is the TCON on and bit 30 the
 *  gamma table on; the table at 0x400, 256 words R<<16 | G<<8 | B, the output for that input. It holds random values
 *  until filled and reads back as nothing useful while on, so it is filled with the table off, then turned on.
 *  Nothing is done while the TCON is off. Header only, on a page the caller gives (/dev/mem on the radio,
 *  panel_stripes.c), so a host test (tools/panel_stripes) can check it on a fake one; it may define TCON_RD and
 *  TCON_WR.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TCON0_BASE          0x01C0C000u     /* TCON0, lcd-controller@1c0c000 in the device tree */
#define TCON0_SIZE          0x1000u
#define TCON_GCTL           0x000u          /* the global control */
#define TCON_GCTL_ON        (1u << 31)      /* the TCON on: the kernel's */
#define TCON_GCTL_GAMMA     (1u << 30)      /* the gamma table on */
#define TCON_GAMMA_TABLE    0x400u
#define TCON_GAMMA_N        256

#define STRIPES_LOW         8               /* the values that stripe on such a panel, in any channel */
#define STRIPES_HIGH        31
#define STRIPES_TO          32              /* sent out instead: the next value that does not */

#ifndef TCON_RD
#define TCON_RD(t, off)     ((t)[(off) / 4])
#define TCON_WR(t, off, v)  ((t)[(off) / 4] = (v))
#endif

typedef enum {
    TCON_GAMMA_NO_TCON = 0,     /* the TCON is off: nothing done */
    TCON_GAMMA_ON,              /* table A filled and on */
    TCON_GAMMA_OFF,             /* the table off */
} tcon_gamma_t;

/* Table A: what goes out for a value in, in each channel */
static inline uint8_t tcon_gamma_a(int v) {
    return v >= STRIPES_LOW && v <= STRIPES_HIGH ? STRIPES_TO : (uint8_t)v;
}

/* Its entry for input i: the same in R, G and B */
static inline uint32_t tcon_gamma_entry(int i) {
    uint32_t v = tcon_gamma_a(i);

    return v << 16 | v << 8 | v;
}

/* On: the table off, filled with table A, then on; every time, as a table on cannot be read back. Off: the table off,
 * the global control's other bits as they are, nothing written when it is off already */
static inline tcon_gamma_t tcon_gamma_set(volatile uint32_t *tcon, bool on) {
    uint32_t g = TCON_RD(tcon, TCON_GCTL);

    if (!(g & TCON_GCTL_ON)) {
        return TCON_GAMMA_NO_TCON;
    }
    if (g & TCON_GCTL_GAMMA) {
        TCON_WR(tcon, TCON_GCTL, g & ~TCON_GCTL_GAMMA);
    }
    if (!on) {
        return TCON_GAMMA_OFF;
    }
    for (int i = 0; i < TCON_GAMMA_N; i++) {
        TCON_WR(tcon, TCON_GAMMA_TABLE + 4 * i, tcon_gamma_entry(i));
    }
    TCON_WR(tcon, TCON_GCTL, TCON_RD(tcon, TCON_GCTL) | TCON_GCTL_GAMMA);
    return TCON_GAMMA_ON;
}

/* With the fix on: the TCON on and its table not, so something reset the display engine since */
static inline bool tcon_gamma_lost(volatile uint32_t *tcon) {
    uint32_t g = TCON_RD(tcon, TCON_GCTL);

    return (g & TCON_GCTL_ON) && !(g & TCON_GCTL_GAMMA);
}

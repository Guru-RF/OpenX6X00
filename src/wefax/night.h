/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax charts at night: a chart is white paper, at night it dazzles. Night Auto dims the charts when
 *  the sun is down at the station's locator (civil twilight, 6 degrees under the horizon: src/sun.h), On and Off
 *  force it; the live picture, the tiles and the full screen dim their grey by WEFAX_NIGHT_DIM. From OlivLoze's fork
 *  (F4OLI), the sun's place from our sun.c.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFAX_NIGHT_DIM 107         /* of 256: white comes out a dark grey */
#define WEFAX_NIGHT_CLOCK_SET 1700000000    /* a clock before Nov 2023 is not set: never dim by guess */

typedef enum {
    WEFAX_NIGHT_AUTO = 0,
    WEFAX_NIGHT_ON,
    WEFAX_NIGHT_OFF,
    WEFAX_NIGHT_MODES
} wefax_night_mode_t;

/* Dim the charts at now: On, Off, or Auto by the sun at the locator qth; Auto never dims without a valid locator or
 * a set clock */
bool wefax_night_at(int mode, const char *qth, time_t now);

/* A grey level as drawn, dimmed or not */
static inline uint8_t wefax_night_grey(uint8_t g, bool night)
{
    return night ? (uint8_t)((g * WEFAX_NIGHT_DIM) >> 8) : g;
}

#ifdef __cplusplus
}
#endif

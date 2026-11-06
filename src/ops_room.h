/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The Ops Room theme's own pieces (a theme with "ops_room": true, theme_shape.ops_room): the clock
 *  as an activity timer in 7-segment digits, INTERNAL or EXTERNAL power over it and the battery's
 *  time left under it; the S-meter as a row of hexagons; a red TRANSMIT band over the waterfall
 *  while the radio transmits.
 */
#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

/* The power by the rules of the other themes' power row (clock.c power_get) */
typedef struct {
    int     h, m, s;
    uint8_t cap;        /* battery % */
    bool    ext;        /* on external power */
    int     est_min;    /* minutes left (or to full), -1 none or made on the other supply */
    bool    approx;     /* the estimate is rough: "~" */
    bool    known;      /* a reading of the power yet */
    bool    battery;    /* a battery there */
    bool    full;       /* on a supply at 100% */
    bool    low;        /* on battery under the guard level */
    float   v_ext;      /* the supply's voltage */
} ops_room_clock_t;

/* Once, with the main screen's object: the band at TX */
void ops_room_init(lv_obj_t *screen);

/* The clock box (clock.c), the S-meter box (smeter.c): drawn over their frames */
void ops_room_clock_draw(lv_obj_t *o, lv_draw_ctx_t *draw_ctx, const ops_room_clock_t *c);
/* db, peak: the bar; value: the number over it, as the other themes' label (the reading as read); tx: "TX" there */
void ops_room_meter_draw(lv_obj_t *o, lv_draw_ctx_t *draw_ctx, float db, float peak, float value, bool tx);

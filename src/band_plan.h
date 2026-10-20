/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  IARU Region 1 band plan (160 m .. 10 m, 6 m) as a thin coloured strip at
 *  the bottom of the spectrum (top of the waterfall when the spectrum is off), lined up with the frequency row.
 */

#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAND_PLAN_H 18

/* Strip on parent (800 px wide) at y; call after the waterfall is created so it is on top */
typedef enum { BP_CW, BP_DIG, BP_ALL, BP_BCN } bp_type_t;   /* the segments: CW, digital, all modes, beacons */

void band_plan_init(lv_obj_t *parent, lv_coord_t y);

/* Move the strip (spectrum off: it follows the top of the waterfall) */
void band_plan_set_y(lv_coord_t y);

/* Span shown: center - half_width .. center + half_width */
void band_plan_update(int32_t center, int32_t half_width);

/* The setting changed: redraw with the last span */
void band_plan_refresh(void);

/* The segment type (bp_type_t) at freq, -1 between the segments */
int band_plan_type(int32_t freq);

/* freq (Hz) is in a CW segment */
bool band_plan_is_cw(int32_t freq);

#ifdef __cplusplus
}
#endif

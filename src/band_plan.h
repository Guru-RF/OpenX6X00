/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The band plan (160 m .. 10 m, 6 m: IARU Region 1, 2 or 3, or the US sub-bands of a licence class) and the bands
 *  of the band list it lacks, as a thin coloured strip at the bottom of the spectrum (top of the waterfall when the
 *  spectrum is off), lined up with the frequency row.
 */

#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAND_PLAN_H 18

/* The segments: CW, digital, all modes, beacons, and (a US plan) outside the licence class */
typedef enum { BP_CW, BP_DIG, BP_ALL, BP_BCN, BP_OUT } bp_type_t;

/* The plans, as Settings > General > Band plan lists them (band_region); Auto takes the region from where the
 * radio is. US Technician is Novice too */
typedef enum {
    BP_PLAN_AUTO, BP_PLAN_R1, BP_PLAN_R2, BP_PLAN_R3,
    BP_PLAN_US_EXTRA, BP_PLAN_US_ADVANCED, BP_PLAN_US_GENERAL, BP_PLAN_US_TECH,
    BP_PLAN_COUNT
} bp_plan_t;

/* Strip on parent (800 px wide) at y; call after the waterfall is created so it is on top */
void band_plan_init(lv_obj_t *parent, lv_coord_t y);

/* Move the strip (spectrum off: it follows the top of the waterfall) */
void band_plan_set_y(lv_coord_t y);

/* Span shown: center - half_width .. center + half_width */
void band_plan_update(int32_t center, int32_t half_width);

/* Segment i of the plan (start, end in Hz, bp_type_t): false past the last one. Any thread */
bool band_plan_segment(int i, int32_t *start, int32_t *end, int *type);

/* The setting changed: redraw with the last span */
void band_plan_refresh(void);

/* The band list may have changed: the plan's segments, and the active bands they do not cover as segments of the
 * type their name says (CW, DIG/FT8/DATA, BCN, else all modes). LVGL thread */
void band_plan_reload(void);

/* The segment type (bp_type_t) at freq, -1 between the segments */
int band_plan_type(int32_t freq);

/* freq (Hz) is in a CW segment */
bool band_plan_is_cw(int32_t freq);

#ifdef __cplusplus
}
#endif

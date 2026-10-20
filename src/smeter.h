/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdint.h>
#include "lvgl/lvgl.h"

/* The S scale in dB: 6 dB an S unit, S9 at -73 dBm, then dB over S9 */
#define S_MIN   (-127)
#define S1      (-121)
#define S2      (S1 + 6)
#define S3      (S1 + 12)
#define S4      (S1 + 18)
#define S5      (S1 + 24)
#define S6      (S1 + 30)
#define S7      (S1 + 36)
#define S8      (S1 + 42)
#define S9      (S1 + 48)
#define S9_20   (S9 + 20)
#define S9_40   (S9 + 40)

/* The S-meter box: bars, needle or the deck scales, by theme and Settings */
lv_obj_t *meter_init(lv_obj_t *parent);

/* From the DSP: a new reading in dB and its smoothing (0 none, towards 1 slower) */
void meter_update(float db, float beta);

/* From the DSP: the noise floor in the filter, in dB; the bars under it stay grey */
void meter_set_noise(float db);

/* The needle dial on any box (the walkie screen), at the current reading */
void meter_needle_draw(lv_obj_t *obj, lv_draw_ctx_t *draw_ctx);

/* The last reading in dB, S1 to S9+40 */
int16_t meter_get_raw_db(void);

/* The same, unrounded (the front end measurement in Settings) */
float meter_get_db(void);

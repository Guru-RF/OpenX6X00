/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - CW tuning indicator
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include <lvgl/lvgl.h>
#include <stdbool.h>

/* The bars on the main screen, shown in CW and CW-R with the tune setting on */
void cw_tune_init(lv_obj_t *parent);

/* Audio thread (cw.cpp): key tone minus the received tone, in Hz, as the X6100 hears it: in CW the dial minus
 * the station, the bar lit on the station's side; in CW-R the other way round. CW on the X6200, whose tone rises
 * as the dial goes up, sends it turned round */
void cw_tune_set_freq(float hz);

/* VISION: the bars at y (the CW decoder panel's top line), -1 back where the style puts them */
void cw_tune_set_y(lv_coord_t y);

/* The highest it goes where no panel places it: under the band plan, on the waterfall */
void cw_tune_set_floor(lv_coord_t y);

/* Any thread: the last offset (Hz, as cw_tune_set_freq() had it) when it came in the
 * last second, for the remote clients */
bool cw_tune_recent(int *hz);

/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdint.h>

typedef enum {
    CLOCK_TIME_ALLWAYS = 0,
    CLOCK_TIME_POWER,
    CLOCK_POWER_ALLWAYS
} clock_view_t;

lv_obj_t * clock_init(lv_obj_t * parent);
void clock_update_power(float ext, float bat, uint8_t cap, bool charge);
float clock_battery_volts(void);
void clock_power_get(float *ext, float *bat, uint8_t *cap, bool *charge);
/* Battery time left or to full from bat_est.c, -1 none; approx shows "~" */
void clock_update_estimate(int minutes, bool approx);

void clock_tz_init();
void clock_tz_set(const char *posix);


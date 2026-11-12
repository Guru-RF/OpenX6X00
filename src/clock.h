/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

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
/* The base found no battery: a red cross in place of the battery and the % */
void clock_update_no_battery(bool none);

/* The hour as the clock shows it: 0-23, or 1-12 with Settings > General > DATE / TIME > 12-hour clock, ampm then
 * " AM" or " PM" ("" with the 24-hour clock, #54) */
int clock_hour(int h, const char **ampm);
/* A local time's hours and minutes so: "15:07", or "3:07 PM" (#54); into buf */
const char *clock_hm(const struct tm *tm, char *buf, size_t size);

void clock_tz_init();
void clock_tz_set(const char *posix);


/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
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

/* The time zones offered (Settings > General > DATE / TIME, the first-start guide): a name and its POSIX TZ string for
 * clock_tz_set(). UTC first, then west to east; the strings differ, as the zone in use is found by its string */
typedef struct {
    const char *label;
    const char *posix;
} clock_zone_t;

extern const clock_zone_t clock_zones[];
extern const size_t       clock_zones_n;

/* The zone in use (TZ) in clock_zones, 0 (UTC) when it is none of them */
size_t clock_zone_now(void);

/* The date and time fields (Settings > General > DATE / TIME, the first-start guide): day, month, year, hour, minute,
 * second, each shown as a struct tm field + base, from lo to hi in digits */
typedef struct {
    int     base;
    int     lo, hi;
    uint8_t digits;
} clock_field_t;

extern const clock_field_t clock_fields[6];

/* The struct tm field of clock_fields[i] */
int *clock_field(struct tm *tm, int i);

/* The fields edited from was to now: the system clock moved by what the edit changed, from where the clock is now (the
 * fields hold the time they were filled with, the clock went on). False when it changed nothing or the clock could not
 * be set */
bool clock_fields_set(const struct tm *was, const struct tm *now);

/* The system clock into the clock chip, in UTC, as the kernel reads it at boot. False when it could not be written:
 * tried again later */
bool clock_rtc_save(void);


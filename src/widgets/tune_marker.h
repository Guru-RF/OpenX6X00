/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - tuning marker over an app waterfall
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#ifndef LV_FINDER_H
#define LV_FINDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/* A band from value to value + width Hz, laid over an audio span of
 * range_min .. range_max Hz, plus an optional second band (the cursor,
 * e.g. a selected station) of the same width. */
typedef struct {
    lv_obj_t    obj;
    int16_t     range_min;
    int16_t     range_max;
    int16_t     value;
    uint16_t    width;
    int16_t     cursor;     /* -1 = none */
} lv_finder_t;

extern const lv_obj_class_t lv_finder_class;

lv_obj_t * lv_finder_create(lv_obj_t * parent);

void lv_finder_set_range(lv_obj_t * obj, int16_t min, int16_t max);
void lv_finder_set_cursor(lv_obj_t * obj, int16_t value);
void lv_finder_clear_cursor(lv_obj_t * obj);
void lv_finder_set_width(lv_obj_t * obj, uint16_t x);
void lv_finder_set_value(lv_obj_t * obj, int16_t x);

#ifdef __cplusplus
}
#endif

#endif

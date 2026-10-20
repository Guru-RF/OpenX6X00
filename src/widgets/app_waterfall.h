/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - scrolling waterfall image for the apps
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#ifndef LV_WATERFALL_H
#define LV_WATERFALL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

#include <time.h>

/* An image whose newest row is on top. Rows live in a buffer twice the
 * image height, every row written twice (h rows apart), and the image
 * descriptor points at the h rows starting at the newest one: a new row
 * moves that pointer up by one row instead of moving the picture. */
typedef struct {
    lv_img_t        img;
    lv_img_dsc_t    dsc;
    lv_color_t      *rows;      /* 2 x h rows of w pixels, NULL before set_size */
    uint32_t        top;        /* row of the newest line, 0 .. h - 1 */
    lv_color_t      *palette;   /* 256 levels, weakest first */
    int16_t         min;        /* dB shown as palette[0] */
    int16_t         max;        /* dB shown as palette[255] */
} lv_waterfall_t;

extern const lv_obj_class_t lv_waterfall_class;

lv_obj_t * lv_waterfall_create(lv_obj_t * parent);

void lv_waterfall_set_palette(lv_obj_t * obj, lv_color_t * palette, uint16_t cnt);
void lv_waterfall_set_size(lv_obj_t * obj, lv_coord_t w, lv_coord_t h);

void lv_waterfall_clear_data(lv_obj_t * obj);
void lv_waterfall_add_data(lv_obj_t * obj, float * data, uint16_t cnt);

/* Same as lv_waterfall_add_data; the timestamp is not used */
void lv_waterfall_add_data_with_ts(lv_obj_t * obj, float * data, uint16_t cnt, struct timespec ts);

void lv_waterfall_set_min(lv_obj_t * obj, int16_t val);
void lv_waterfall_set_max(lv_obj_t * obj, int16_t val);

#ifdef __cplusplus
}
#endif

#endif

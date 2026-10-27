/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The 1bit theme's drawing (theme_shape.mono): everything LVGL draws on this display comes out in
 *  the theme's ink or paper (theme_shape.ink, .paper), at alpha 0 or 255. Other themes: LVGL's own
 *  renderer, untouched.
 *
 *  Copyright (c) 2026 RF.Guru
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* disp_drv.draw_ctx_init: LVGL's software renderer, with the mono hooks when mono is on (the
 * screen's context and lv_snapshot's) */
void mono_draw_ctx_init(lv_disp_drv_t *drv, lv_draw_ctx_t *ctx);

/* styles_load(): the hooks and the LVGL widget theme on or off, the two colours from theme_shape */
void mono_set(bool on);

/* The nearer of ink and paper by luma (128 and up: paper), alpha 0xff */
lv_color_t mono_snap(lv_color_t c);

/* Ordered dither, 8x8 Bayer: true where a pixel of density 0 (paper) .. 255 (ink) is ink */
static inline bool mono_dither(uint8_t density, int x, int y) {
    static const uint8_t bayer[8][8] = {
        { 0, 32,  8, 40,  2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44,  4, 36, 14, 46,  6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
        { 3, 35, 11, 43,  1, 33,  9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47,  7, 39, 13, 45,  5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
    };
    return density > 4 * bayer[y & 7][x & 7] + 2;
}

#ifdef __cplusplus
}
#endif

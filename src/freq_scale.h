/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Frequency row under the spectrum: the edges of the displayed span and
 *  round reference frequencies in between, lined up with the waterfall.
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Labels on parent (BOARD_DISP_W px wide) at y */
void freq_scale_init(lv_obj_t *parent, lv_coord_t y);
/* The labels to y (the spectrum height changed) */
void freq_scale_set_y(lv_coord_t y);
void freq_scale_foreground(void);   /* Amber Trey: over the display */
void freq_scale_set_width(lv_coord_t w);   /* the right edge label at w (Amber Trey, CW: half the screen) */

/* Span from center - half_width to center + half_width across the width */
/* Dashed lines under the reference frequencies, over the waterfall */
void freq_scale_lines_init(lv_obj_t *parent, lv_coord_t y, lv_coord_t h);

/* The top to y, the bottom where it was */
void freq_scale_lines_set_y(lv_coord_t y);

void freq_scale_update(int32_t center, int32_t half_width, uint32_t color);

/* The reference frequencies shown: their x (0..799) and Hz, at most max; how many */
int freq_scale_refs(int32_t *x, int32_t *hz, int max);

/* The dashed lines hidden (the 3D waterfall draws its own) or shown with their labels */
void freq_scale_lines_hide(bool hide);

#ifdef __cplusplus
}
#endif

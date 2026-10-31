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

/* Labels on parent (800 px wide) at y */
void freq_scale_init(lv_obj_t *parent, lv_coord_t y);
/* The labels to y (the spectrum height changed) */
void freq_scale_set_y(lv_coord_t y);
void freq_scale_foreground(void);   /* KX3Lovers Plus: over the display */
void freq_scale_set_width(lv_coord_t w);   /* the right edge label at w (Plus, CW: half the screen) */

/* Span from center - half_width to center + half_width across the width */
/* Dashed lines under the reference frequencies, over the waterfall */
void freq_scale_lines_init(lv_obj_t *parent, lv_coord_t y, lv_coord_t h);

/* The top to y, the bottom where it was */
void freq_scale_lines_set_y(lv_coord_t y);

void freq_scale_update(int32_t center, int32_t half_width, uint32_t color);

#ifdef __cplusplus
}
#endif

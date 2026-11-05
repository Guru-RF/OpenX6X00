/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Amber Trey: the radio's state on an amber segment LCD (lcd_panel.c)
 *
 *  Copyright (c) 2026 Pavol Calfa OM0WT
 */

#pragma once

#include "lvgl/lvgl.h"

/* Over the main screen from the top, height px tall */
lv_obj_t *lcd_panel_init(lv_obj_t *parent, lv_coord_t height);

/* Amber Trey: where the spectrum strip is */
#define LCD_PLUS_LCD_Y      24      /* the segment LCD over it: its top, */
#define LCD_PLUS_LCD_H      286     /* its height (the layout, stretched) */
#define LCD_PLUS_STRIP_Y    294
#define LCD_PLUS_STRIP_H    112
#define LCD_PLUS_STRIP_W    800

/* Amber Trey: the spectrum strip, in CW while keying or while decoded text comes in the left half,
 * the text in the right half */
void lcd_panel_set_strip(lv_obj_t *spectrum);

/* Any thread: CW decoder text, shown in the strip's lower half (Amber Trey) */
void lcd_panel_cw_text(const char *text);

/* spectrum_view.c: the auto levels lifted for the short strip (the floor a quarter up, not on its edge) */
void lcd_panel_levels(float *min, float *max);

/* spectrum_view.c: its strip of points columns h high (column x at (points - 1 - x) * h) squeezed into
 * the first w, the rest bg; nothing when w is the full width (every theme but Amber Trey in CW) */
void lcd_panel_squeeze(uint32_t *strip, int h, int points, int w, uint32_t bg);

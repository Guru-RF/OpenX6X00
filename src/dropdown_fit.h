/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a long dropdown list kept whole (#310)
 *
 *  LVGL 8 keeps an object's height under LV_COORD_MAX (8191 px: the coordinates are 16 bits, three of them flags).
 *  A dropdown's list is one label with every option on its own line, so a list taller than that is cut at 8191 px:
 *  the knob still moves the choice down the list, but the list stops scrolling and the choice goes on out of sight.
 *  Settings > General > Country and the first-start guide's Country step, 246 lines at the theme's spacing (39 px a
 *  line in Settings), stopped at Suriname. Each time such a list opens, its lines come closer together, as much as it
 *  takes to fit; a shorter list keeps the theme's spacing. styles_dropdown_list() (gui_styles.c) gives it to every
 *  dropdown of the GUI.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The spacing between `lines` lines of a font `font_h` px high that keeps them under LV_COORD_MAX: `space` (the
 * theme's) when that fits, less when not, 0 at the least */
lv_coord_t dropdown_fit_space(uint32_t lines, lv_coord_t font_h, lv_coord_t space);

/* dd's list fitted each time it opens */
void dropdown_fit(lv_obj_t *dd);

#ifdef __cplusplus
}
#endif

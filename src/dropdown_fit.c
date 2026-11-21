/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a long dropdown list kept whole (#310): see dropdown_fit.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "dropdown_fit.h"

lv_coord_t dropdown_fit_space(uint32_t lines, lv_coord_t font_h, lv_coord_t space) {
    if (lines < 2) return space;

    /* LVGL's label: the lines, and the spacing between them (none after the last) */
    int32_t gaps = ((int32_t)LV_COORD_MAX - (int32_t)lines * font_h) / (int32_t)(lines - 1);

    if (gaps >= space) return space;
    return gaps > 0 ? gaps : 0;
}

/* LVGL's dropdown sends READY as its list opens, before the list's label takes the options and its size: the
 * spacing set here is the one the label, the highlight of the choice and the scroll to it all go by. The theme's
 * spacing is read again each time, for the font and the options the list has now */
static void ready_cb(lv_event_t *e) {
    lv_obj_t *dd   = lv_event_get_current_target(e);
    lv_obj_t *list = lv_dropdown_get_list(dd);

    lv_obj_remove_local_style_prop(list, LV_STYLE_TEXT_LINE_SPACE, 0);

    lv_coord_t space = lv_obj_get_style_text_line_space(list, 0);
    lv_coord_t fit   = dropdown_fit_space(lv_dropdown_get_option_cnt(dd),
                                          lv_font_get_line_height(lv_obj_get_style_text_font(list, 0)), space);

    if (fit != space) lv_obj_set_style_text_line_space(list, fit, 0);
}

void dropdown_fit(lv_obj_t *dd) {
    lv_obj_add_event_cb(dd, ready_cb, LV_EVENT_READY, NULL);
}

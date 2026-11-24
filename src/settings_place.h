/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Settings opens where it was left (#362)
 *
 *  Going back and forth between a setting and the screen it changes, Settings opened on General's first row each
 *  time, and the tab and the row had to be found again. Now it opens on the tab it was closed on, the knob on the row
 *  it was on. The row is kept by what it is, not by where it was: the card's title, the row's label, and which of the
 *  controls with those two it was (a row of two dropdowns, two Volume rows in one card), with the name of a list's
 *  entry (a network, a device) for a list. A row added or gone above it leaves it found; a row that is gone itself,
 *  hidden or disabled now, or a label in another language, is not found and the tab opens at its top. A tab chosen
 *  starts at its top as before. Kept in memory until the radio is off: a new power-on opens on General.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A row's label (column 0 of its card's grid, settings_window.cpp's card_end() marks it) */
#define SETTINGS_ROW_LABEL  LV_OBJ_FLAG_USER_1

typedef struct {
    char       key[192];    /* card title, row label, a list entry's name, apart by 0x1f; "" none kept */
    int        n;           /* the n-th control on the page with that key, 0 the first */
    lv_coord_t scroll_y;    /* how far the page was scrolled */
} settings_place_t;

/* The label of obj's row on its card, NULL with none */
lv_obj_t *settings_row_label(lv_obj_t *obj);

/* The knob can land on obj: it is not disabled, and neither it nor any parent hidden */
bool settings_reachable(lv_obj_t *obj);

/* Where obj, a control of group g, is on page. False, place emptied, when obj is not one of page's */
bool settings_place_get(settings_place_t *place, lv_obj_t *page, lv_group_t *g, lv_obj_t *obj);

/* The control at place on page (built again since): NULL when it is not there or out of the knob's reach */
lv_obj_t *settings_place_find(const settings_place_t *place, lv_obj_t *page, lv_group_t *g);

#ifdef __cplusplus
}
#endif

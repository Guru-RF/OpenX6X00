/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  DX cluster column on the main screen: the spots of the current band,
 *  around a VFO marker row, in a box at the right of the waterfall.
 */

#pragma once

#include <stdbool.h>

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Box on parent (800 px wide), from y to the bottom buttons; call after the waterfall */
void cluster_column_init(lv_obj_t *parent, lv_coord_t y);

/* The waterfall top moved */
void cluster_column_layout(lv_coord_t y);

/* Setting or VFO changed: redraw */
void cluster_column_refresh(void);

/* The MFK "Cluster" item is active: a highlight row is shown and moved */
void cluster_column_set_active(bool on);

/* Hidden whatever the setting (the snake game plays over the waterfall) */
void cluster_column_hide(bool hide);

/* Moves the highlight by dir spots (clamped); returns the highlighted call ("" if none) */
const char *cluster_column_move(int dir);

/* Tunes the radio to the highlighted spot */
void cluster_column_tune(void);

/* The box on its parent, if shown (the spot calls on the waterfall keep out of it) */
bool cluster_column_area(lv_area_t *a);

#ifdef __cplusplus
}
#endif

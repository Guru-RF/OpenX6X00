// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *  Xiegu X6100 LVGL GUI - how a row of the FT8/FT4/FT2 list looks
 *
 *  What decode_list.c paints a row with, from what the row holds and the app's CQ rows (page 5, #359): Edge, F4OLI's
 *  redesign and the default since v0.3.13, the CQs plain in the text's colour with a green edge and a tag; Filled,
 *  the CQs on green rows as up to v0.3.12, a darker green for a station worked. Roles only, no LVGL drawing: a host
 *  test checks them (tools/ft8_rows). The MFK's row and the 1bit theme's frames stay decode_list.c's.
 */

#pragma once

#include <stdbool.h>

#include "../theme.h"
#include "decode_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* cfg_ft8_cq_rows */
typedef enum {
    FT8_CQ_ROWS_EDGE = 0,   /* F4OLI's redesign, the default */
    FT8_CQ_ROWS_FILLED,     /* the green rows of v0.3.12 */
} ft8_cq_rows_t;

typedef struct {
    theme_color_t bg;           /* the row's ground, at the list's own opacity */
    theme_color_t text;         /* TC_COUNT: the list's own text colour */
    lv_opa_t      text_opa;
    bool          strike;       /* struck through: a CQ worked on this band and mode */
    bool          center;       /* a note or a slot line, in the middle */
    lv_opa_t      edge;         /* the CQ's green edge on the left, 0 none */
} ft8_row_look_t;

/* cd NULL: the "Wait sync" row. mono: the 1bit theme, where the green rows are paper (theme.c) and a CQ is marked
 * by a frame: it keeps the redesign's look whatever cq_rows says */
ft8_row_look_t ft8_row_look(const cell_data_t *cd, ft8_cq_rows_t cq_rows, bool mono);

#ifdef __cplusplus
}
#endif

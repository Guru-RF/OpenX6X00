/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The decoder text panel over the waterfall: CW decoder, RTTY, NavTex */
lv_obj_t *panel_init(lv_obj_t *parent);

void panel_hide(void);
void panel_clear(void);                       /* the text, unless the panel is hidden */
void panel_update_visibility(bool clear);     /* shown for the mode and the decoders, cleared if asked */
void panel_add_text(const char *text);        /* any thread: decoded text, '\n' a new line */
void panel_set_info(const char *text);        /* any thread: the line above the panel (CW WPM) */

/* The spectrum got taller: the panel starts at y, shorter */
void panel_keep_below(lv_coord_t y);

/* The last callsign the decoder wrote (CW / RTTY panel), "" when none */
const char *panel_last_call(void);

#ifdef __cplusplus
}
#endif

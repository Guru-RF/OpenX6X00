/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Frequency strip: a bar under the top bar with band, VFO frequency,
 *  locator and mode, always shown.
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *freq_strip_init(lv_obj_t *parent);

/* Shows text instead of the band while the VFO stays on freq_hz. */
void      freq_strip_set_label(const char *text, int32_t freq_hz);

/* Shows dial + offset_hz (FT8 TX audio offset) as the frequency; offset_hz < 0 = plain dial. */
void      freq_strip_set_offset(int32_t offset_hz);
/* Name of the open app, shown before the band (NULL: none) */
void      freq_strip_set_app(const char *name);
/* An app window opens (true) or closes: the deck's frequency moves up, out from under it */
void      freq_strip_set_dialog(bool open);

#ifdef __cplusplus
}
#endif

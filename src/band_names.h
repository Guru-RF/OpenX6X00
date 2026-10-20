/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* Band names over the top of the waterfall after a frequency change (band_info.cpp) */

#ifndef X6100_BAND_INFO_H
#define X6100_BAND_INFO_H

#include "lvgl/lvgl.h"
#include <stdint.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *band_info_init(lv_obj_t *waterfall);
/* The VFO moved to freq: redraw and show for a while */
void band_info_update(int32_t freq);

#ifdef __cplusplus
}
#endif

#endif

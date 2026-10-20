/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

void screen_rec_toggle();
bool screen_rec_is_on();
void screen_rec_shutdown();
/* Display flush_cb: keeps a copy of the screen while recording, then draws it with screen_rec_out */
void screen_rec_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);
/* The display backend flush: fbdev_flush (default) or drm_flush */
extern void (*screen_rec_out)(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);
/* Called from the audio thread */
void screen_rec_put_audio_samples(size_t nsamples, int16_t *samples);

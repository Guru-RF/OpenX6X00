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
#include <unistd.h>

#include "lvgl/lvgl.h"

lv_obj_t *waterfall_init(lv_obj_t *parent);
/* The height left under the spectrum: at start, and again when the spectrum changes */
void waterfall_set_height(lv_coord_t h);

/* A row from the DSP thread: size dB values over width_hz centred on base_freq (0: not known) */
void waterfall_data(float *db, uint16_t size, bool tx, uint32_t base_freq, uint32_t width_hz);

/* The colour scale: from the band's settings, or (auto levels) from the DSP */
void waterfall_min_max_reset();
void waterfall_update_min(float db);
void waterfall_update_max(float db);
/* Its limits (dB) for the rows, the reference level applied */
void waterfall_get_levels(bool tx, float *min, float *max);

/* Fixed mode (cfg_wf_mode): the displayed span stays put and the VFO moves across it.
 * Returns how far (Hz) the centre of the received data is from the display centre;
 * always 0 in Centered mode. Re-centres the view on the received data when they are a
 * quarter of the spectrum's span (100 kHz / zoom) from its centre, or the VFO 40%. */
int32_t waterfall_view_shift(void);

/* LV_EVENT_DRAW_MAIN_END of an object drawn on the DRM primary plane (waterfall,
 * spectrum): its area of the GUI fully transparent, whatever is behind it */
void direct_hole_cb(lv_event_t *e);

/* Dialogs draw the waterfall one refresh in k, and back to every one */
void waterfall_refresh_period_set(uint8_t k);
void waterfall_refresh_reset();

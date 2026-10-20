/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "cfg/encoder_bind_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

#include <unistd.h>

/* The knob lines over the buttons: made on the main screen (again after a theme change) */
void knobs_init(lv_obj_t *parent);

/* Whether VOL / MFK change the value (edit) or choose the control */
void knobs_set_vol_state(bool edit);
void knobs_set_mfk_state(bool edit);

/* Shown when on and enabled in Settings (off while a dialog covers them) */
void knobs_display(bool on);
bool knobs_visible();

/* For an app that draws the knobs itself (Field Ops): what VOL (vol true) or MFK controls, its value,
 * its level 0..1 (-1: no scale) and whether turning changes the value (else it chooses the control) */
bool knobs_info(bool vol, const char **name, char *val, size_t size, float *level, bool *edit);

#ifdef __cplusplus
}

/* What VOL / MFK control now */
void knobs_set_vol_param(cfg_ctrl_t control);
void knobs_set_mfk_param(cfg_ctrl_t control);
#endif

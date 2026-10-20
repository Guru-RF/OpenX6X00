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

#include <stdbool.h>
#include <stdint.h>

/* Turning VOL changes the value (edit) or chooses the control (select) */
typedef enum {
    VOL_STATE_EDIT = 0,
    VOL_STATE_SELECT,
} vol_state_t;

void vol_init();

/* As mfk_update(), mfk_change_ctrl(), mfk_set_ctrl() for the VOL knob */
void vol_update(int16_t diff);
void vol_change_ctrl(int16_t dir);
void vol_set_ctrl(cfg_ctrl_t mode);

/* What it controls */
cfg_ctrl_t vol_get_ctrl();

#ifdef __cplusplus
}
#endif

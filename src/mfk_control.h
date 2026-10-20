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

/* Turning the MFK changes the value (edit) or chooses the control (select) */
typedef enum {
    MFK_STATE_EDIT = 0,
    MFK_STATE_SELECT
} mfk_state_t;

extern mfk_state_t mfk_state;
extern cfg_ctrl_t  mfk_ctrl;        /* what it controls */

void mfk_init();

/* Turn the value by diff steps (0: show it only) */
void mfk_update(int16_t diff);

/* The next (dir > 0) or previous control bound to the MFK */
void mfk_change_ctrl(int16_t dir);

/* Give it this control, in edit state, and show the value */
void mfk_set_ctrl(cfg_ctrl_t mode);

/* The same without the message */
void mfk_set_ctrl_quiet(cfg_ctrl_t mode);

/* The cluster column went away: leave its controls */
void mfk_cluster_gone();

#ifdef __cplusplus
}
#endif

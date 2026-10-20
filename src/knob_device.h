/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "gui_events.h"
#include "lvgl/lvgl.h"

#include <stdint.h>

/* The VOL knob: a turn left or right sends left[state] or right[state] (keys_t) */
typedef struct {
    int            fd;
    uint16_t       left[3];
    uint16_t       right[3];
    uint8_t        state;           /* vol_state_t */
    lv_indev_drv_t indev_drv;
    lv_indev_t    *indev;
} rotary_t;

/* EVENT_ROTARY param of the main knob (freed by the receiver): detents and
 * the time per detent in ms (0: unknown, e.g. from a keyboard) */
typedef struct {
    int16_t         diff;
    uint16_t        dt;
} rotary_data_t;

/* Open the VOL knob's /dev/input/eventN; NULL if it cannot */
rotary_t *rotary_init(char *dev_name);

/* Open the main knob's device and start its reader thread */
void rotary_main_init(char *dev_name);

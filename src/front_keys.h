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

/* One button device (the radio has two) */
typedef struct {
    int            fd;
    lv_indev_drv_t indev_drv;
    lv_indev_t    *indev;
    int            evdev_state;     /* the VOL press as LVGL sees it */
    int            evdev_key;
} keypad_t;

/* Opens /dev/input/eventN for the buttons; NULL if it cannot */
keypad_t *keypad_init(char *dev_name);

/* Bottom buttons: a hold is a press longer than this many ms (0: back to 1 s), for an app
 * that wants quicker holds while it is open */
void keypad_set_long_time(uint32_t hold);

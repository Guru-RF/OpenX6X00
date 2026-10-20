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

/* The MFK knob */
typedef struct {
    int            fd;
    bool           pressed;         /* set by the buttons' device */
    lv_indev_drv_t indev_drv;
    lv_indev_t    *indev;
} encoder_t;

/* Open the MFK's /dev/input/eventN; NULL if it cannot */
encoder_t *encoder_init(char *dev_name);

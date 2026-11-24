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
#include "mfk_rings.h"

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

/* The X6200's two rings (mfk_rings.h, #354). The inner ring's device, once: without it there is one ring, the MFK */
void mfk_rings_inner(int fd);
/* Settings > Display > INTERFACE > MFK rings is Swapped, on a radio with two rings */
bool mfk_rings_swapped(void);
/* What the ring that has the role now sent since the last read, as clicks, a turn to the left positive; *turned is
 * set when it sent anything. The GUI thread */
int32_t mfk_rings_read(mfk_role_t role, bool *turned);
/* The MFK proper turned n clicks from a browser (positive to the right) while Swapped. Any thread */
void mfk_rings_inject(int n);

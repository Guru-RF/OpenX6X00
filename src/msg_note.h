/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The big note in the middle of the screen (msg_tiny.c) */

#ifndef X6100_MSG_TINY_H
#define X6100_MSG_TINY_H

#include "lvgl/lvgl.h"
#include <stdint.h>
#include <unistd.h>

lv_obj_t *msg_tiny_init(lv_obj_t *parent);
void msg_tiny_set_text_fmt(const char *fmt, ...);

#endif

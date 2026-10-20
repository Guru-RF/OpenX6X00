/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The message box (msg.c): printf formats, "#rrggbb text#" colours a part */

#ifndef X6100_MSG_H
#define X6100_MSG_H

#include "lvgl/lvgl.h"
#include <stdint.h>
#include <unistd.h>

lv_obj_t *msg_init(lv_obj_t *parent);

/* Shown now, over whatever is shown, for 2 s */
void msg_update_text_fmt(const char *fmt, ...);
/* Shown after the messages waiting, for 2 s / 4 s / ms */
void msg_schedule_text_fmt(const char *fmt, ...);
void msg_schedule_long_text_fmt(const char *fmt, ...);
void msg_schedule_text_ms(uint16_t ms, const char *fmt, ...);

#endif

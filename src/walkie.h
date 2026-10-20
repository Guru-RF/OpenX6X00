/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 */

#pragma once

#include "lvgl/lvgl.h"

/* The portrait Walkie Talkie screen (480x800) in place of main_screen_obj while cfg_walkie is on,
 * followed live; keys it does not use itself go to the main screen */
void walkie_init(lv_obj_t *main_screen_obj);

/* The walkie screen is up: the VOL knob is the volume only, the buttons' knob is the MFK */
bool walkie_on(void);
/* A theme change: the screen, built once, is built again when next shown */
void walkie_drop(void);

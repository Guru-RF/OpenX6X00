/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* Screen backlight and front key lights (backlight.c) */

#ifndef X6100_BACKLIGHT_H
#define X6100_BACKLIGHT_H

#include "params/params.h"
#include <stdbool.h>
#include <stdint.h>

void backlight_init();
/* Every key and knob calls it: the idle time starts again. True when the screen was blanked
 * and this woke it: the key was for waking, the radio should not see it */
bool backlight_tick();

/* Level 0-9 now (the settings), below 0: off */
void backlight_set_brightness(int16_t level);
/* The front keys' light setting, saved */
void backlight_set_buttons(buttons_light_t mode);

/* POWER key: the screen off, or back on */
void backlight_switch();

/* The level shown now (0-9 as the settings), -1 when the screen is off */
int16_t backlight_level();

/* screen_off_min or screen_off_always changed: applied now */
void backlight_settings_changed();
/* A remote viewer (remote_server.c) is watching the screen or stopped: the GUI keeps drawing
 * for it while the panel is dark */
void backlight_viewers(bool any);
/* The panel is dark (POWER, the blank time or always off) */
bool backlight_is_dark();

#endif

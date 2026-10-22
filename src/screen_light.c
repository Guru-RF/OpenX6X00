/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

/* The screen's backlight and the light of the front keys: dimmed after a time without a key
 * or knob, back at the next one; the POWER key turns the screen off and on.
 *
 * OpenX6X00: the screen goes dark after screen_off_min minutes without a key or knob (0: never),
 * the key that wakes it is swallowed; with screen_off_always it stays dark (a radio run from the
 * VFO-Knob or a browser), only the POWER key shows it for a while. While the panel is dark LVGL
 * stops drawing, unless a remote viewer (remote_server.c) is looking at the screen. */

#include "screen_light.h"
#include "lvgl/lvgl.h"
#include "util.h"

#include "radio.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SYS_BL "/sys/class/backlight/backlight/"

static int         power_fd, level_fd;
static bool        screen_on = true;    /* the POWER key's toggle */
static bool        blanked;             /* dark after the idle time (or always off) */
static bool        viewers;             /* a remote viewer: drawn even when dark */
static int16_t     shown = -1;          /* backlight_level() */
static lv_timer_t *idle_timer, *blank_timer;

/* The driver counts the other way: 10 - level */
static void write_level(int16_t level) {
    char text[8];
    int  n = snprintf(text, sizeof(text), "%i\n", 10 - level);

    if (level_fd > 0 && write(level_fd, text, n) < 0) {
        LV_LOG_WARN("Backlight level not set");
    }
}

/* bl_power: 0 on, 1 off */
static void write_power(bool on) {
    if (power_fd > 0 && write(power_fd, on ? "0\n" : "1\n", 2) < 0) {
        LV_LOG_WARN("Backlight power not set");
    }
}

static void keys_light(bool on) {
    radio_backlight_power(on);
}

static bool dark(void) {
    return !screen_on || blanked;
}

/* Nothing drawn while the panel is dark and nobody is watching from afar */
static void drawing_update(void) {
    lv_disp_enable_invalidation(lv_disp_get_default(), !dark() || viewers);
    if (!dark() || viewers) {
        lv_obj_invalidate(lv_scr_act());
    }
}

static void panel_dark(void) {
    write_power(false);
    write_level(9);
    keys_light(false);
    shown = -1;
    drawing_update();
}

static void panel_lit(void) {
    write_power(true);
    write_level(params.brightness_normal);
    shown = params.brightness_normal;
    keys_light(params.brightness_buttons != BUTTONS_DARK);
    drawing_update();
}

static void idle_cb(lv_timer_t *t) {
    idle_timer = NULL;
    backlight_set_brightness(params.brightness_idle);
    keys_light(params.brightness_buttons == BUTTONS_LIGHT);
}

static void blank_cb(lv_timer_t *t) {
    blank_timer = NULL;
    if (!blanked) {
        blanked = true;
        if (screen_on) {
            panel_dark();
        }
    }
}

/* The blank timer: screen_off_min minutes, or a minute when the screen is always off and POWER
 * showed it; none when it never blanks */
static void blank_arm(void) {
    uint32_t min = params.screen_off_always ? (params.screen_off_min ? params.screen_off_min : 1) : params.screen_off_min;

    if (blank_timer) {
        lv_timer_del(blank_timer);
        blank_timer = NULL;
    }
    if (min == 0) {
        return;
    }
    blank_timer = lv_timer_create(blank_cb, min * 60000, NULL);
    lv_timer_set_repeat_count(blank_timer, 1);
}

void backlight_init() {
    power_fd  = open(SYS_BL "bl_power", O_WRONLY);
    level_fd  = open(SYS_BL "brightness", O_WRONLY);
    screen_on = true;
    if (params.screen_off_always) {
        blanked = true;
        panel_dark();
        return;
    }
    backlight_tick();
}

/* A key or a knob: the idle time starts again, the light comes back if it was dimmed.
 * True when the screen was blanked and this woke it: the key is for waking, not for the radio */
bool backlight_tick() {
    uint32_t ms = params.brightness_timeout * 1000;

    if (blanked) {
        if (params.screen_off_always || !screen_on) {
            return false;       /* dark on purpose: the keys work in the dark */
        }
        blanked = false;
        panel_lit();
        blank_arm();
        if (idle_timer) {
            lv_timer_set_period(idle_timer, ms);
            lv_timer_reset(idle_timer);
        } else {
            idle_timer = lv_timer_create(idle_cb, ms, NULL);
            lv_timer_set_repeat_count(idle_timer, 1);
        }
        return true;
    }
    if (screen_on && !params.screen_off_always) {
        blank_arm();
    }
    if (idle_timer) {
        lv_timer_set_period(idle_timer, ms);
        lv_timer_reset(idle_timer);
        return false;
    }
    idle_timer = lv_timer_create(idle_cb, ms, NULL);
    lv_timer_set_repeat_count(idle_timer, 1);
    backlight_set_brightness(params.brightness_normal);
    keys_light(params.brightness_buttons != BUTTONS_DARK);
    return false;
}

/* 0-9; below 0 the light goes off (PWM at its top: less noise) */
void backlight_set_brightness(int16_t value) {
    if (dark()) {
        return;
    }
    if (value < 0) {
        shown = -1;
        write_power(false);
        write_level(9);
    } else {
        shown = value;
        write_level(value);
        write_power(true);
    }
}

void backlight_set_buttons(buttons_light_t value) {
    params_lock();
    params.brightness_buttons = value;
    params_unlock(&params.dirty.brightness_buttons);
    keys_light(value != BUTTONS_DARK);
}

int16_t backlight_level() {
    return shown;
}

/* POWER: dark when lit (by the key or by the timer), lit when dark. With the screen always off it
 * stays lit for the blank time, a minute at least */
void backlight_switch() {
    if (dark()) {
        screen_on = true;
        blanked   = false;
        panel_lit();
        blank_arm();
    } else {
        screen_on = false;
        if (blank_timer) {
            lv_timer_del(blank_timer);
            blank_timer = NULL;
        }
        panel_dark();
    }
}

/* The screen off settings changed (Settings, the web configurator) */
void backlight_settings_changed() {
    if (params.screen_off_always) {
        if (!blanked) {
            blanked = true;
            if (screen_on) {
                panel_dark();
            }
        }
        if (blank_timer) {
            lv_timer_del(blank_timer);
            blank_timer = NULL;
        }
        return;
    }
    if (blanked && screen_on) {
        blanked = false;
        panel_lit();
    }
    blank_arm();
}

void backlight_viewers(bool any) {
    viewers = any;
    drawing_update();
}

bool backlight_is_dark() {
    return dark();
}

/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The screen's backlight and the light of the front keys: dimmed after a time without a key
 * or knob, back at the next one; the POWER key turns the screen off and on */

#include "screen_light.h"
#include "lvgl/lvgl.h"
#include "util.h"

#include <aether_radio/x6100_control/low/gpio.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SYS_BL "/sys/class/backlight/backlight/"

static int         power_fd, level_fd;
static bool        screen_on = true;
static int16_t     shown = -1;          /* backlight_level() */
static lv_timer_t *idle_timer;

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
    x6100_gpio_set(x6100_pin_light, on ? 1 : 0);
}

static void idle_cb(lv_timer_t *t) {
    idle_timer = NULL;
    backlight_set_brightness(params.brightness_idle);
    keys_light(params.brightness_buttons == BUTTONS_LIGHT);
}

void backlight_init() {
    power_fd  = open(SYS_BL "bl_power", O_WRONLY);
    level_fd  = open(SYS_BL "brightness", O_WRONLY);
    screen_on = true;
    backlight_tick();
}

/* A key or a knob: the idle time starts again, the light comes back if it was dimmed */
void backlight_tick() {
    uint32_t ms = params.brightness_timeout * 1000;

    if (idle_timer) {
        lv_timer_set_period(idle_timer, ms);
        lv_timer_reset(idle_timer);
        return;
    }
    idle_timer = lv_timer_create(idle_cb, ms, NULL);
    lv_timer_set_repeat_count(idle_timer, 1);
    backlight_set_brightness(params.brightness_normal);
    keys_light(params.brightness_buttons != BUTTONS_DARK);
}

/* 0-9; below 0 the light goes off (PWM at its top: less noise) */
void backlight_set_brightness(int16_t value) {
    if (!screen_on) {
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

void backlight_switch() {
    if (screen_on) {
        write_power(false);
        write_level(9);
        keys_light(false);
        screen_on = false;
        shown = -1;
        lv_disp_enable_invalidation(lv_disp_get_default(), false);   /* nothing drawn while dark */
    } else {
        lv_disp_enable_invalidation(lv_disp_get_default(), true);
        write_power(true);
        write_level(params.brightness_normal);
        shown = params.brightness_normal;
        keys_light(params.brightness_buttons != BUTTONS_DARK);
        screen_on = true;
    }
}

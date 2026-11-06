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
 * OpenX6X00: after screen_saver_min minutes without use the screensaver (screen_saver.c), and
 * the screen dark after screen_off_min minutes (0: never); the key that wakes it is swallowed.
 * With screen_off_always it stays dark (a radio run from the VFO-Knob or a browser), only the
 * POWER key shows it for a while. When is screen_idle.c's; here what it says is done. While the
 * panel is dark or the screensaver is over the GUI, LVGL draws nothing of the GUI, unless a
 * remote viewer (remote_server.c) is looking at the screen or it is recorded (screen_rec.c). */

#include "screen_light.h"
#include "screen_idle.h"
#include "screen_rec.h"
#include "screen_saver.h"
#include "spectrum_view.h"
#include "waterfall_view.h"
#include "lvgl/lvgl.h"
#include "util.h"

#include "radio.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SYS_BL "/sys/class/backlight/backlight/"
#define STAGE_MS 1000       /* the minutes, the radio's state and a recording looked at every second */
#define KNOB_MS  500        /* the knob turn that ends the screensaver: its detents for so long only end it */

static int            power_fd, level_fd;
static screen_idle_t  idle;                 /* the stage and its times */
static screen_stage_t stage = SCREEN_LIT;   /* the stage shown */
static bool           drawing = true;       /* LVGL's invalidation on (its count: one step a change) */
static bool           viewers;              /* a remote viewer: drawn even when dark */
static int16_t        shown = -1;           /* backlight_level() */
static lv_timer_t    *idle_timer;
static uint64_t       knob_woke_ms;         /* a knob ended the screensaver then (backlight_knob_tick) */
static bool           knob_woke;

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
    return stage == SCREEN_DARK;
}

/* The screen watched from afar, or recorded: the GUI drawn for them, dark or not, and no screensaver */
static bool watched(void) {
    return viewers || screen_rec_is_on();
}

/* Nothing of the GUI drawn while the panel is dark or under the screensaver, and nobody is watching from
 * afar. LVGL counts the calls: one only when it changes (with each viewer's coming and going it counted up,
 * and a dark screen was drawn on). Seen again: all of it drawn, the waterfall and the spectrum now too, not
 * from their next row */
static void drawing_update(void) {
    bool on = stage == SCREEN_LIT || watched();

    if (on == drawing) {
        return;
    }
    drawing = on;
    lv_disp_enable_invalidation(lv_disp_get_default(), on);
    if (on) {
        lv_obj_invalidate(lv_scr_act());
        waterfall_redraw();
        spectrum_redraw();
    }
}

/* The GUI drawn once under the screensaver as it goes dark: the rain left in the buffers would be what the next
 * wake shows first, until the GUI's first picture. The loop flips it in while the panel is dark (drm_commit) */
static void gui_picture(void) {
    lv_disp_enable_invalidation(lv_disp_get_default(), true);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_disp_enable_invalidation(lv_disp_get_default(), false);
}

static void panel_dark(void) {
    write_power(false);
    write_level(9);
    keys_light(false);
    shown = -1;
}

static void panel_lit(void) {
    write_power(true);
    write_level(params.brightness_normal);
    shown = params.brightness_normal;
    keys_light(params.brightness_buttons != BUTTONS_DARK);
}

/* The stage screen_idle.c came to, shown: the panel dark or lit, the screensaver on or off */
static void stage_show(void) {
    screen_stage_t was = stage;

    if (idle.stage == was) {
        return;
    }
    stage = idle.stage;
    if (was == SCREEN_SAVER) {
        screen_saver_hide();
        if (stage == SCREEN_DARK && !drawing) {
            gui_picture();
        }
    }
    if (stage == SCREEN_DARK) {
        panel_dark();
    } else if (was == SCREEN_DARK) {
        panel_lit();
    }
    if (stage == SCREEN_SAVER) {
        screen_saver_show(params.screen_saver.x);
    }
    drawing_update();
}

static void idle_cb(lv_timer_t *t) {
    idle_timer = NULL;
    backlight_set_brightness(params.brightness_idle);
    keys_light(params.brightness_buttons == BUTTONS_LIGHT);
}

/* The idle time again, the light back at its level if it was dimmed */
static void light_again(void) {
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

/* The radio's state as well as its TX events: the SWR scan sends none, and transmits until stopped */
static bool on_air(void) {
    radio_state_t s = radio_get_state();

    return s != RADIO_RX && s != RADIO_POWEROFF && s != RADIO_OFF;
}

static void stage_cb(lv_timer_t *t) {
    bool air = on_air(), seen = watched();

    if (air != idle.tx) {
        backlight_tx(air);
    }
    if (seen != idle.viewers) {
        screen_idle_viewers(&idle, seen);   /* a recording started or stopped */
    }
    screen_idle_tick(&idle, get_time());
    stage_show();
    drawing_update();
}

/* The settings as screen_idle.c takes them, in their ranges */
static void idle_settings(void) {
    idle.saver      = params.screen_saver.x <= SCREEN_SAVER_LAST ? params.screen_saver.x : SCREEN_SAVER_NONE;
    idle.saver_min  = LV_CLAMP(1, params.screen_saver_min.x, SCREEN_MIN_MAX);
    idle.off_min    = LV_MIN(params.screen_off_min, SCREEN_MIN_MAX);
    idle.always_off = params.screen_off_always;
}

void backlight_init() {
    power_fd = open(SYS_BL "bl_power", O_WRONLY);
    level_fd = open(SYS_BL "brightness", O_WRONLY);
    idle_settings();
    screen_idle_start(&idle, get_time());
    lv_timer_create(stage_cb, STAGE_MS, NULL);
    if (idle.stage == SCREEN_DARK) {
        stage_show();
        return;
    }
    backlight_tick();
}

/* A key or a knob: the minutes start again, the light comes back if it was dimmed.
 * True when the screensaver or a screen dark by the time is gone: the key was for waking, not for the radio */
bool backlight_tick() {
    bool woke = screen_idle_use(&idle, get_time());

    stage_show();
    if (!dark()) {
        light_again();      /* dark on purpose: the keys work in the dark */
    }
    return woke;
}

bool backlight_saver_on() {
    return stage == SCREEN_SAVER;
}

bool backlight_saver_tick() {
    bool saver = backlight_saver_on();

    return backlight_tick() && saver;
}

/* A knob is read every 10 ms: the detents of the turn that ended the screensaver, read in the next loops, only end
 * it too, for KNOB_MS */
bool backlight_knob_tick() {
    uint64_t now = get_time();

    if (backlight_saver_tick()) {
        knob_woke    = true;
        knob_woke_ms = now;
        return true;
    }
    if (knob_woke && now - knob_woke_ms < KNOB_MS) {
        return true;
    }
    knob_woke = false;
    return false;
}

/* With a screensaver: on the air it is gone at once, TX in view as after a key */
void backlight_tx(bool on) {
    bool saver = stage == SCREEN_SAVER;

    screen_idle_tx(&idle, on, get_time());
    stage_show();
    if (saver && stage == SCREEN_LIT) {
        light_again();
    }
}

/* As TX: the screensaver gone, the warning in view */
void backlight_notice() {
    bool saver = stage == SCREEN_SAVER;

    screen_idle_notice(&idle, get_time());
    stage_show();
    if (saver && stage == SCREEN_LIT) {
        light_again();
    }
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

/* POWER: dark when lit (by the key or by the timer, the screensaver too), lit when dark. With the screen always
 * off it stays lit for the screen off time, a minute at least */
void backlight_switch() {
    screen_idle_power(&idle, get_time());
    stage_show();
}

/* The screen off and screensaver settings changed (Settings, the web configurator): the minutes again */
void backlight_settings_changed() {
    idle_settings();
    screen_idle_settings(&idle, get_time());
    stage_show();
}

void backlight_viewers(bool any) {
    viewers = any;
    screen_idle_viewers(&idle, watched());
    stage_show();
    drawing_update();
}

bool backlight_is_dark() {
    return dark();
}

bool backlight_seen() {
    return stage == SCREEN_LIT || watched();
}

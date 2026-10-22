/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "clock.h"
#include "board.h"

#include "styles.h"
#include "theme.h"
#include "backlight.h"
#include "msg.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/stat.h>

static lv_obj_t         *obj;
static lv_obj_t         *time_label;
static lv_obj_t         *sec_label;
static lv_obj_t         *power_labels[4];  /* battery symbol, volts, ext symbol, volts or % */
static pthread_mutex_t  power_mux;

static lv_timer_t       *timer_time_sync;
static time_t           last_time_sync=0;

static float            v_ext;
static float            v_bat;
static uint8_t          cap_bat;
static bool             charging;
static int              est_min = -1;
static bool             est_approx;

static void check_time_sync_cb(lv_timer_t * t);
static time_t get_last_sync_time();

/* Time on top (seconds smaller), voltages below, both always shown. */
static void show_time() {
    time_t      now;
    struct tm   *t;

    now = time(NULL);
    t = localtime(&now);

    lv_label_set_text_fmt(time_label, "%02i:%02i", t->tm_hour, t->tm_min);
    lv_label_set_text_fmt(sec_label, ":%02i", t->tm_sec);

    pthread_mutex_lock(&power_mux);
    const char * bat_sym;
    const char * ext_sym;
    /* From the base's %, not the voltage: the pack sits at 7.0-7.2 V from 25% down to 5%
     * (battery_log.csv), so the old 7.0 V step showed half a battery at 10% */
    switch (cap_bat) {
        case 0 ... 9:
            bat_sym = LV_SYMBOL_BATTERY_EMPTY;
            break;
        case 10 ... 34:
            bat_sym = LV_SYMBOL_BATTERY_1;
            break;
        case 35 ... 59:
            bat_sym = LV_SYMBOL_BATTERY_2;
            break;
        case 60 ... 84:
            bat_sym = LV_SYMBOL_BATTERY_3;
            break;
        default:
            bat_sym = LV_SYMBOL_BATTERY_FULL;
            break;
    }
    if (charging) {
        ext_sym = LV_SYMBOL_CHARGE;
    } else {
        ext_sym = LV_SYMBOL_POWER;
    }

    /* The % and the time left or to full (bat_est.c); the voltages are in Settings > Info */
    if (v_bat < 3.0f) {     /* external power, no battery */
        lv_label_set_text(power_labels[0], "");
        lv_label_set_text(power_labels[1], "");
        lv_label_set_text(power_labels[2], ext_sym);
        lv_label_set_text_fmt(power_labels[3], "%.1fv", v_ext);
    } else {
        bool ext = v_ext >= 3.0f;

        lv_label_set_text(power_labels[0], ext && est_min >= 0 ? LV_SYMBOL_CHARGE : bat_sym);
        lv_label_set_text_fmt(power_labels[1], "%i%%", cap_bat);
        lv_label_set_text(power_labels[2], ext && est_min < 0 ? LV_SYMBOL_POWER : "");  /* charged */
        if (est_min >= 0) {
            lv_label_set_text_fmt(power_labels[3], "%s%ih%02i", est_approx ? "~" : "", est_min / 60, est_min % 60);
        } else {
            lv_label_set_text(power_labels[3], "");
        }

        /* On battery below the guard level (20% or under half an hour left): red */
        bool low = !ext && (cap_bat <= 20 || (est_min >= 0 && est_min < 30));

        for (int i = 0; i < 4; i++)
            lv_obj_set_style_text_color(power_labels[i], low ? TC(tx)
                                        : (theme_shape.deck && i % 2) ? TC(text_mute) : TC(text_soft), 0);
    }

    pthread_mutex_unlock(&power_mux);
}

lv_obj_t * clock_init(lv_obj_t * parent) {
    if (!obj) pthread_mutex_init(&power_mux, NULL);

    obj = lv_obj_create(parent);

    lv_obj_add_style(obj, &clock_style, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(obj);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 0);

    time_label = lv_label_create(row);
    lv_obj_set_style_text_font(time_label, theme_shape.vision ? &sony_40 : &sony_38, 0);

    sec_label = lv_label_create(row);
    lv_obj_set_style_text_font(sec_label, &sony_22, 0);
    lv_obj_set_style_pad_bottom(sec_label, 2, 0);
    if (theme_shape.vision)
        lv_obj_add_flag(sec_label, LV_OBJ_FLAG_HIDDEN);

    /* Voltages: symbols in Montserrat, numbers in Sony */
    row = lv_obj_create(obj);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -3);

    for (int i = 0; i < 4; i++) {
        power_labels[i] = lv_label_create(row);
        lv_obj_set_style_text_font(power_labels[i], i % 2 ? &sony_24 : &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(power_labels[i], TC(text_soft), 0);
    }
    lv_obj_set_style_pad_left(power_labels[2], 4, 0);
    if (theme_shape.vision) {   /* VISION: the time only here, the battery in a box of its own at the right end of the S-meter's row */
        lv_obj_t *box = lv_obj_create(parent);

        lv_obj_add_style(box, &clock_style, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(box, VISION_POWER_W, METER_ROW_H - 2 * TOP_BAR_MARGIN);
        lv_obj_set_y(box, FREQ_STRIP_Y + FREQ_STRIP_H + TOP_BAR_MARGIN);
        lv_obj_set_parent(row, box);
        lv_obj_center(row);
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_text_font(power_labels[i], i % 2 ? &sony_20 : &lv_font_montserrat_22, 0);
    }
    if (theme_shape.deck) {     /* "13:03:02" at the right of the first row, the voltages in the second */
        lv_obj_set_style_text_font(time_label, &sony_24, 0);
        lv_obj_set_style_text_font(sec_label, &sony_24, 0);
        lv_obj_set_style_pad_bottom(sec_label, 0, 0);
        lv_obj_align(lv_obj_get_parent(time_label), LV_ALIGN_TOP_RIGHT, 0, 2);
        lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, -3);
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_text_font(power_labels[i], i % 2 ? &sony_20 : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(power_labels[1], TC(text_mute), 0);
        lv_obj_set_style_text_color(power_labels[3], TC(text_mute), 0);
    }

    show_time();
    if (!timer_time_sync) {     /* not again on a theme change */
        lv_timer_create(show_time, 500, NULL);
        last_time_sync = get_last_sync_time();
        timer_time_sync = lv_timer_create(check_time_sync_cb, 200, NULL);
    }
    return obj;
}

void clock_update_power(float ext, float bat, uint8_t cap, bool charge_flag) {
    pthread_mutex_lock(&power_mux);
    v_ext = ext;
    v_bat = bat;
    cap_bat = cap;
    charging = charge_flag;
    pthread_mutex_unlock(&power_mux);
}

void clock_update_estimate(int minutes, bool approx) {
    pthread_mutex_lock(&power_mux);
    est_min = minutes > 5999 ? 5999 : minutes;
    est_approx = approx;
    pthread_mutex_unlock(&power_mux);
}

static void check_time_sync_cb(lv_timer_t * t) {
    time_t new_time = get_last_sync_time();
    if (last_time_sync != new_time) {
        last_time_sync = new_time;
        msg_schedule_text_fmt("Time is synchronized with NTP");
    }
}

static time_t get_last_sync_time() {
    struct stat attr;
    if (stat("/var/run/time-sync", &attr) == 0) {
        return attr.st_atime;
    }
    return 0;
}

/* Time zone: POSIX TZ string kept in /mnt/tz.conf, the system clock stays UTC */

#define TZ_FILE BOARD_DATA "/tz.conf"

void clock_tz_init() {
    char  line[64] = "UTC0";
    FILE *f = fopen(TZ_FILE, "r");

    if (f) {
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
        }
        fclose(f);
    }
    setenv("TZ", line[0] ? line : "UTC0", 1);
    tzset();
}

void clock_tz_set(const char *posix) {
    FILE *f = fopen(TZ_FILE, "w");

    if (f) {
        fprintf(f, "%s\n", posix);
        fclose(f);
    }
    setenv("TZ", posix, 1);
    tzset();
    if (time_label) {
        show_time();
    }
}

void clock_power_get(float *ext, float *bat, uint8_t *cap, bool *charge) {
    pthread_mutex_lock(&power_mux);
    *ext = v_ext;
    *bat = v_bat;
    *cap = cap_bat;
    *charge = charging;
    pthread_mutex_unlock(&power_mux);
}

float clock_battery_volts(void) {
    return v_bat;
}

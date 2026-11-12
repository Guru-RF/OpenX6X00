/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "clock.h"
#include "ops_room.h"
#include "board.h"

#include "styles.h"
#include "theme.h"
#include "backlight.h"
#include "msg.h"
#include "cfg/cfg_api.h"
#include "params/params.h"

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

#define POWER_TURN_S    5       /* Power display Alternate: each for 5 s */

/* Montserrat has the battery and the bolt but no plug, for external power without charging:
 * fonts/power_sym_*.c add it and fall back to Montserrat for the rest */
extern const lv_font_t  power_sym_14, power_sym_18, power_sym_22;
#define SYMBOL_PLUG     "\xEF\x87\xA6"  /* U+F1E6 */

static pthread_mutex_t  power_mux;

static lv_timer_t       *timer_time_sync;
static time_t           last_time_sync=0;

static float            v_ext;
static float            v_bat;
static uint8_t          cap_bat;
static bool             charging;
static int              est_min = -1;
static bool             est_approx;
static bool             est_ext;        /* the estimate was made on external power */
static bool             no_battery;     /* the base found no battery */
static lv_coord_t       power_col = 4;  /* the power row's gaps: between the labels, and more before the supply's */
static lv_coord_t       power_half = 4; /* (power_gaps) */

static void check_time_sync_cb(lv_timer_t * t);
static time_t get_last_sync_time();

/* % + V on a supply is the widest choice: "100%" with both signs and the supply's voltage is wider than the
 * box with the wider fonts (DejaVu, the default, and Sony in VISION's box). Then the gaps narrow, the one
 * between the two halves last: 4 px between the labels and 4 more before the supply's sign when there is room,
 * else 2 or 1 between the labels and what is left (up to 4 more) before the supply's sign. The usual gaps in
 * the other choices */
static void power_gaps(bool fit) {
    lv_obj_t  *row  = lv_obj_get_parent(power_labels[0]);
    lv_obj_t  *box  = lv_obj_get_parent(row);
    lv_coord_t col  = 4, half = 4;

    if (fit) {
        /* the box's width inside its border, 2 px clear of each side, less the labels */
        lv_coord_t room = lv_obj_get_style_width(box, 0) - 2 * lv_obj_get_style_border_width(box, 0) - 4;

        for (int i = 0; i < 4; i++) {
            const char *t = lv_label_get_text(power_labels[i]);

            room -= lv_txt_get_width(t, strlen(t), lv_obj_get_style_text_font(power_labels[i], 0), 0,
                                     LV_TEXT_FLAG_NONE);
        }
        if (room < 3 * col + half) {
            col  = room >= 3 * 2 + 4 ? 2 : 1;
            half = LV_CLAMP(0, room - 3 * col, 4);
        }
    }
    if (col != power_col || half != power_half) {
        power_col  = col;
        power_half = half;
        lv_obj_set_style_pad_column(row, col, 0);
        lv_obj_set_style_pad_left(power_labels[2], half, 0);
    }
}

/* The power as the clocks show it, from the last readings: power_mux held */
typedef struct {
    bool known;         /* a reading yet: radio.c gives the first some seconds after start */
    bool battery;       /* a battery there: not when the base says none, nor under 3 V */
    bool ext;           /* on a supply */
    bool full;          /* on a supply at 100%: nothing to full */
    bool chg;           /* charging */
    bool low;           /* on battery under the guard level: 20%, or under half an hour left */
    int  est;           /* minutes to empty, on a supply to full; -1 none, or made before the supply changed */
} power_t;

static power_t power_get(void) {
    power_t p = {.known = v_bat >= 3.0f || v_ext >= 3.0f, .battery = !no_battery && v_bat >= 3.0f, .est = -1};

    p.ext = !p.battery || v_ext >= 3.0f;
    if (!p.known || !p.battery) return p;
    /* The estimate comes once a minute, to empty on battery or to full on external power. If the supply changed
     * since, it is the wrong one: hidden until the next. At 100%, nothing to full */
    p.full = p.ext && cap_bat >= 100;
    p.est  = est_ext == p.ext && !p.full ? est_min : -1;
    /* Charging is the base's flag (radio.c drops it when the charger is off) or a time to full, which rides over a
     * reading where the flag blinks off */
    p.chg = !p.full && (p.est >= 0 || charging);
    p.low = !p.ext && (cap_bat <= 20 || (p.est >= 0 && p.est < 30));
    return p;
}

/* The hour as the clock shows it: 0-23, or with the 12-hour clock 1-12, *ampm then " AM" or " PM" (#54) */
int clock_hour(int h, const char **ampm) {
    *ampm = "";
    if (!params.clock_12h.x) return h;
    *ampm = h < 12 ? " AM" : " PM";
    return h % 12 ? h % 12 : 12;
}

const char *clock_hm(const struct tm *tm, char *buf, size_t size) {
    const char *ampm;
    int         h = clock_hour(tm->tm_hour, &ampm);

    snprintf(buf, size, ampm[0] ? "%i:%02i%s" : "%02i:%02i%s", h, tm->tm_min, ampm);
    return buf;
}

/* Time on top (seconds smaller), voltages below, both always shown. */
static void show_time() {
    time_t      now;
    struct tm   *t;

    now = time(NULL);
    t = localtime(&now);
    if (theme_shape.ops_room) {     /* drawn in draw_cb */
        lv_obj_invalidate(obj);
        return;
    }

    const char *ampm;
    int         h = clock_hour(t->tm_hour, &ampm);

    /* 12-hour: "1:05" and ":30 PM" (12:34:56 PM is 154 px of the 168); VISION has no seconds, the AM or PM in
     * their place (12:34 PM is 130 px of its 132) */
    lv_label_set_text_fmt(time_label, *ampm ? "%i:%02i" : "%02i:%02i", h, t->tm_min);
    if (theme_shape.vision) {
        lv_label_set_text(sec_label, *ampm ? ampm + 1 : "");
        if (*ampm) lv_obj_clear_flag(sec_label, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(sec_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text_fmt(sec_label, ":%02i%s", t->tm_sec, ampm);
    }

    int  show = param_i_get(cfg_power_display);
    bool volts = show == 1 || (show == 2 && now / POWER_TURN_S % 2);   /* else the % and the time */
    bool pct_v = show == 3;     /* % + V: the %, and on a supply the supply's voltage in place of the time */
    bool fit   = false;         /* % + V on a supply: the gaps fitted to the box (power_gaps) */

    pthread_mutex_lock(&power_mux);
    const char * bat_sym;
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

    /* The % and the time left or to full (bat_est.c), the voltages, the two in turn, or the % and the
     * supply's voltage: Settings > Display > MAIN SCREEN > Power display. Without a battery the supply's
     * voltage, whatever it says */
    if (v_bat < 3.0f && v_ext < 3.0f) {    /* no reading yet: radio.c gives the first some seconds after start */
        for (int i = 0; i < 4; i++)
            lv_label_set_text(power_labels[i], "");
    } else if (no_battery) {        /* a red cross in place of the battery, as Xiegu's app shows it, and
                                     * the plug: nothing to charge, whatever the charging flag says */
        lv_label_set_text(power_labels[0], LV_SYMBOL_CLOSE);
        lv_label_set_text(power_labels[1], "");
        lv_label_set_text(power_labels[2], SYMBOL_PLUG);
        lv_label_set_text_fmt(power_labels[3], "%.1fv", v_ext);
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_text_color(power_labels[i], i == 0 ? TC(tx)
                                        : (theme_shape.deck && i % 2) ? TC(text_mute) : TC(text_soft), 0);
    } else if (v_bat < 3.0f) {      /* external power, no battery */
        lv_label_set_text(power_labels[0], "");
        lv_label_set_text(power_labels[1], "");
        lv_label_set_text(power_labels[2], SYMBOL_PLUG);
        lv_label_set_text_fmt(power_labels[3], "%.1fv", v_ext);
    } else {
        power_t pw  = power_get();
        bool    ext = pw.ext;
        int     est = pw.est;

        /* The symbol before the %: the bolt while charging, the plug on external power without
         * charging (full, or the charger set off), the battery level on battery (power_get) */
        bool chg = pw.chg;

        if (volts) {
            /* The voltages: the battery's after its level, on a supply its sign and its voltage */
            lv_label_set_text(power_labels[0], bat_sym);
            lv_label_set_text_fmt(power_labels[1], "%.1fv", v_bat);
            lv_label_set_text(power_labels[2], !ext ? "" : chg ? LV_SYMBOL_CHARGE : SYMBOL_PLUG);
            if (ext) {
                lv_label_set_text_fmt(power_labels[3], "%.1fv", v_ext);
            } else {
                lv_label_set_text(power_labels[3], "");
            }
        } else if (pct_v && ext) {
            /* % + V on a supply (#119): the battery's level and %, then the supply's sign and its voltage
             * as Volts has them. No time to full: no room for it. On battery as % + time, below */
            lv_label_set_text(power_labels[0], bat_sym);
            lv_label_set_text_fmt(power_labels[1], "%i%%", cap_bat);
            lv_label_set_text(power_labels[2], chg ? LV_SYMBOL_CHARGE : SYMBOL_PLUG);
            lv_label_set_text_fmt(power_labels[3], "%.1fv", v_ext);
            fit = true;
        } else {
            lv_label_set_text(power_labels[0], !ext ? bat_sym : chg ? LV_SYMBOL_CHARGE : SYMBOL_PLUG);
            lv_label_set_text_fmt(power_labels[1], "%i%%", cap_bat);
            lv_label_set_text(power_labels[2], "");
            if (est >= 0) {
                lv_label_set_text_fmt(power_labels[3], "%s%ih%02i", est_approx ? "~" : "", est / 60, est % 60);
            } else {
                lv_label_set_text(power_labels[3], "");
            }
        }

        /* On battery below the guard level (20% or under half an hour left): red */
        bool low = pw.low;

        for (int i = 0; i < 4; i++)
            lv_obj_set_style_text_color(power_labels[i], low ? TC(tx)
                                        : (theme_shape.deck && i % 2) ? TC(text_mute) : TC(text_soft), 0);
    }
    power_gaps(fit);

    pthread_mutex_unlock(&power_mux);
}

/* Ops Room: the time in 7-segment digits between INTERNAL / EXTERNAL and the charge, and the time
 * the battery has left as the activity limit under it (ops_room.c draws), by the rules of the other themes'
 * power row (power_get): no battery, the estimate of the other supply hidden, nothing to full at 100% */
static void draw_cb(lv_event_t *e) {
    time_t     now = time(NULL);
    struct tm *t = localtime(&now);

    pthread_mutex_lock(&power_mux);
    power_t          p = power_get();
    const char      *ampm;
    ops_room_clock_t c = {.h = clock_hour(t->tm_hour, &ampm), .m = t->tm_min, .s = t->tm_sec, .cap = cap_bat, .ext = p.ext,
                          .est_min = p.est, .approx = est_approx, .known = p.known, .battery = p.battery,
                          .full = p.full, .low = p.low, .v_ext = v_ext};
    pthread_mutex_unlock(&power_mux);
    ops_room_clock_draw(lv_event_get_target(e), lv_event_get_draw_ctx(e), &c);
}

lv_obj_t * clock_init(lv_obj_t * parent) {
    if (!obj) pthread_mutex_init(&power_mux, NULL);

    obj = lv_obj_create(parent);

    lv_obj_add_style(obj, &clock_style, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    if (theme_shape.ops_room) lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

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
        lv_obj_set_style_text_font(power_labels[i], i % 2 ? &sony_24 : &power_sym_18, 0);
        lv_obj_set_style_text_color(power_labels[i], TC(text_soft), 0);
    }
    lv_obj_set_style_pad_left(power_labels[2], 4, 0);
    power_col  = 4;             /* new labels (a theme change): the usual gaps */
    power_half = 4;
    if (theme_shape.vision) {   /* VISION: the time only here, the battery in a box of its own at the right end of the S-meter's row */
        lv_obj_t *box = lv_obj_create(parent);

        lv_obj_add_style(box, &clock_style, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(box, VISION_POWER_W, METER_ROW_H - 2 * TOP_BAR_MARGIN);
        lv_obj_set_y(box, FREQ_STRIP_Y + FREQ_STRIP_H + TOP_BAR_MARGIN);
        lv_obj_set_parent(row, box);
        lv_obj_center(row);
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_text_font(power_labels[i], i % 2 ? &sony_20 : &power_sym_22, 0);
    }
    if (theme_shape.deck) {     /* "13:03:02" at the right of the first row, the voltages in the second */
        lv_obj_set_style_text_font(time_label, &sony_24, 0);
        lv_obj_set_style_text_font(sec_label, &sony_24, 0);
        lv_obj_set_style_pad_bottom(sec_label, 0, 0);
        lv_obj_align(lv_obj_get_parent(time_label), LV_ALIGN_TOP_RIGHT, 0, 2);
        lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, -3);
        for (int i = 0; i < 4; i++)
            lv_obj_set_style_text_font(power_labels[i], i % 2 ? &sony_20 : &power_sym_14, 0);
        lv_obj_set_style_text_color(power_labels[1], TC(text_mute), 0);
        lv_obj_set_style_text_color(power_labels[3], TC(text_mute), 0);
    }

    if (theme_shape.ops_room) {
        lv_obj_add_flag(lv_obj_get_parent(time_label), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
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
    est_ext = v_ext >= 3.0f;    /* radio.c updates the power of the same reading just before */
    pthread_mutex_unlock(&power_mux);
}

void clock_update_no_battery(bool none) {
    pthread_mutex_lock(&power_mux);
    no_battery = none;
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

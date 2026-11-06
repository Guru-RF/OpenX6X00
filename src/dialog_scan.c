/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Scanner app
 *
 *  The list to scan (Source): the Channels app's channels, the airband (118-137 MHz, AM, 25 kHz:
 *  the X6200), the 40 CB channels (AM), or the band plan segment the dial is in, in steps of the
 *  mode. The engine (scan.c) looks at each in turn and stops on one over the threshold, Auto by
 *  default (a margin over what each reads when quiet); the speaker is quiet between stops.
 *  What it stopped on is listed, the latest first: an evening's activity at a glance.
 *
 *  Skip goes on, Lock out leaves one out for this session (a carrier that is always there), the
 *  MFK steps by hand while paused, the main knob pauses it. Receive only: a TX (the mic's PTT, TCI)
 *  holds it on the one it is on, and nothing retunes while the radio is on the air.
 */

#include "dialog_scan.h"
#include "screen_light.h"

#include "app_frame.h"
#include "band_plan.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "channel_list.h"
#include "dialog.h"
#include "events.h"
#include "keyboard.h"
#include "meter.h"
#include "msg_box.h"
#include "radio.h"
#include "scan.h"
#include "styles.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TICK_MS     20
#define HITS_SHOWN  8
#define LEFT_W      372
#define NAME_MAX    24

typedef enum { SRC_CHANNELS, SRC_AIRBAND, SRC_CB, SRC_SEGMENT, SRC_N } source_t;

static const char *source_name[SRC_N] = {"Channels", "Airband", "CB", "Segment"};

static const struct { const char *name; float db; } stops[] = {
    {"Auto", NAN}, {"S3", S3}, {"S5", S5}, {"S7", S7}, {"S9", S9},
};
static const struct { const char *name; int ms; } speeds[] = {{"Fast", 120}, {"Normal", 250}, {"Slow", 500}};
static const int hangs_s[] = {2, 5, 10};

/* US CB channels 1-40: 23 sits between 22 and 24 out of order, as the FCC numbered them */
static const int32_t cb_khz[40] = {
    26965, 26975, 26985, 27005, 27015, 27025, 27035, 27055, 27065, 27075, 27085, 27105, 27115, 27125,
    27135, 27155, 27165, 27175, 27185, 27205, 27215, 27225, 27255, 27235, 27245, 27265, 27275, 27285,
    27295, 27305, 27315, 27325, 27335, 27345, 27355, 27365, 27375, 27385, 27395, 27405,
};

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void run_cb(button_data_t *b);
static void source_cb(button_data_t *b);
static void skip_cb(button_data_t *b);
static void lock_cb(button_data_t *b);
static void stop_at_cb(button_data_t *b);
static void speed_cb(button_data_t *b);
static void hang_cb(button_data_t *b);
static void clear_cb(button_data_t *b);

static buttons_page_t page_1, page_2;

static button_data_t btn_p1     = {.type = BTN_TEXT, .label = "(SCAN 1:2)", .press = button_next_page_cb,
                                   .hold = button_prev_page_cb, .next = &page_2, .prev = &page_2};
static button_data_t btn_source = {.type = BTN_TEXT, .label = "Source", .press = source_cb};
static button_data_t btn_run    = {.type = BTN_TEXT, .label = "Start", .press = run_cb};
static button_data_t btn_skip   = {.type = BTN_TEXT, .label = "Skip", .press = skip_cb};
static button_data_t btn_lock   = {.type = BTN_TEXT, .label = "Lock\nout", .press = lock_cb};
static button_data_t btn_p2     = {.type = BTN_TEXT, .label = "(SCAN 2:2)", .press = button_next_page_cb,
                                   .hold = button_prev_page_cb, .next = &page_1, .prev = &page_1};
static button_data_t btn_stop   = {.type = BTN_TEXT, .label = "Stop at\nAuto", .press = stop_at_cb};
static button_data_t btn_speed  = {.type = BTN_TEXT, .label = "Speed\nNormal", .press = speed_cb};
static button_data_t btn_hang   = {.type = BTN_TEXT, .label = "Hang\n2 s", .press = hang_cb};
static button_data_t btn_clear  = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};

static buttons_page_t page_1 = {.items = {&btn_p1, &btn_source, &btn_run, &btn_skip, &btn_lock}};
static buttons_page_t page_2 = {.items = {&btn_p2, &btn_stop, &btn_speed, &btn_hang, &btn_clear}};

static dialog_t dialog = {
    .run           = false,
    .construct_cb  = construct_cb,
    .destruct_cb   = destruct_cb,
    .btn_page      = &page_1,
    .key_cb        = key_cb,
    .restore_radio = true,      /* where it was before the scan, on close */
};

dialog_t *dialog_scan = &dialog;

static struct {
    int32_t      freq;
    radio_mode_t mode;
    char         name[NAME_MAX];
} list[SCAN_MAX];

static int        list_n;
static source_t   source = SRC_CHANNELS;
static int        stop_i, speed_i = 1, hang_i;
static app_frame_t frame;
static lv_timer_t *timer;
static lv_obj_t   *lbl_state, *lbl_freq, *lbl_name, *lbl_info, *meter_track, *meter_bar, *meter_thr;
static lv_obj_t   *hit_rows[HITS_SHOWN], *hits_head;
static int32_t     tuned_freq;     /* what the scan tuned last: a different dial is a hand on the knob */
static uint32_t    tuned_seq;      /* meter_seq() then: the readings that may still be of the frequency before */

/* ---- The lists ---- */

static int add(int32_t freq, radio_mode_t mode, const char *name) {
    if (list_n >= SCAN_MAX || !radio_check_freq(freq)) return list_n;
    list[list_n].freq = freq;
    list[list_n].mode = mode;
    snprintf(list[list_n].name, NAME_MAX, "%s", name ? name : "");
    return ++list_n;
}

static void build_list(void) {
    char name[NAME_MAX];

    list_n = 0;
    switch (source) {
        case SRC_CHANNELS:
            channels_reload_if_changed();
            for (uint16_t i = 0; i < channels_count(); i++) {
                const channel_t *c = channels_get(i);
                add(c->freq, c->mode, c->name);
            }
            break;
        case SRC_AIRBAND:
            for (int32_t f = 118000000; f <= 136975000; f += 25000) add(f, radio_mode_am, "");
            break;
        case SRC_CB:
            for (int i = 0; i < 40; i++) {
                snprintf(name, sizeof(name), "CB %d", i + 1);
                add(cb_khz[i] * 1000, radio_mode_am, name);
            }
            break;
        case SRC_SEGMENT: {
            int32_t      dial = cparam_i_get(cfg_fg_freq), from = 0, to = 0, s, e;
            radio_mode_t mode = (radio_mode_t)cparam_i_get(cfg_cur_mode);
            int          type, step;

            for (int i = 0; band_plan_segment(i, &s, &e, &type); i++)
                if (dial >= s && dial < e) {
                    from = s;
                    to   = e;
                }
            step = mode == radio_mode_cw || mode == radio_mode_cwr ? 500 : mode == radio_mode_am ? 5000 : 3000;
            if (to > from)
                for (int32_t f = (from + step - 1) / step * step; f < to; f += step) add(f, mode, "");
            break;
        }
        default:
            break;
    }
    scan_init(list_n);
}

/* ---- Readout ---- */

static void s_text(float db, char *buf, size_t size) {
    if (isnan(db)) snprintf(buf, size, "-");
    else if (db <= S9) snprintf(buf, size, "S%d", (int)LV_CLAMP(1, 1 + floorf((db - S1) / 6), 9));
    else snprintf(buf, size, "S9+%d", (int)roundf(db - S9));
}

static lv_coord_t meter_x(float db) {
    lv_coord_t w = lv_obj_get_width(meter_track);
    return (lv_coord_t)LV_CLAMP(0, (db - S1) / (S9_40 - S1) * w, w);
}

static void fill(void) {
    int          cur = scan_current();
    scan_state_t st  = scan_state();
    char         buf[96], s[12];
    float        db  = meter_get_db(), thr = scan_threshold(cur);

    static const char *state_name[] = {"STOPPED", "SCANNING", "BUSY", "PAUSED"};
    lv_label_set_text(lbl_state, list_n ? state_name[st] : "NOTHING TO SCAN");
    lv_obj_set_style_text_color(lbl_state, st == SCAN_HOLD ? TC(ok) : st == SCAN_SEEKING ? TC(accent) : TC(text_dim),
                                0);

    if (cur >= 0 && cur < list_n) {
        lv_label_set_text_fmt(lbl_freq, list[cur].freq % 1000 ? "%.4f %s" : "%.3f %s", list[cur].freq / 1e6,
                              channels_mode_name(list[cur].mode));
        lv_label_set_text(lbl_name, list[cur].name);
    } else {
        lv_label_set_text(lbl_freq, "");
        lv_label_set_text(lbl_name, "");
    }

    lv_obj_set_width(meter_bar, LV_MAX(2, meter_x(db)));
    lv_obj_set_style_bg_color(meter_bar, !isnan(thr) && db >= thr ? TC(ok) : TC(meter_bar), 0);
    if (isnan(thr)) {
        lv_obj_add_flag(meter_thr, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(meter_thr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(meter_thr, meter_x(thr) - 1);
    }

    int locked = 0;
    for (int i = 0; i < list_n; i++) locked += scan_locked(i);
    s_text(thr, s, sizeof(s));
    snprintf(buf, sizeof(buf), "%d to scan, pass %d.  Stops at %s%s", list_n, scan_passes() + 1,
             isnan(stops[stop_i].db) ? "Auto: " : "", isnan(thr) ? "learning" : s);
    if (locked) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ".  %d locked out", locked);
    lv_label_set_text(lbl_info, buf);

    for (int k = 0; k < HITS_SHOWN; k++) {
        const scan_hit_t *h = scan_hit(k);

        if (!h || h->idx >= list_n) {
            lv_label_set_text(hit_rows[k], "");
            continue;
        }
        struct tm tm;
        char      t[8];

        localtime_r(&h->start, &tm);
        strftime(t, sizeof(t), "%H:%M", &tm);
        s_text(h->peak, s, sizeof(s));
        int32_t f = list[h->idx].freq;

        lv_label_set_text_fmt(hit_rows[k], f % 1000 ? "%s  %8.4f  %-10.10s %5s %3d s" : "%s  %8.3f  %-10.10s %5s %3d s",
                              t, f / 1e6, list[h->idx].name, s, h->secs);
        lv_obj_set_style_text_color(hit_rows[k], k == 0 && st == SCAN_HOLD ? TC(ok) : TC(text), 0);
    }
    lv_label_set_text(hits_head, scan_hits_n() ? "Heard  (latest first)" : "Heard: nothing yet");

    lv_label_set_text_fmt(frame.head_left, "SCANNER   %s", source_name[source]);
    lv_label_set_text_fmt(frame.head_right, "%s  %s  hang %d s", stops[stop_i].name, speeds[speed_i].name,
                          hangs_s[hang_i]);
}

static void labels_refresh(void) {
    static char stop_lbl[20], speed_lbl[20], hang_lbl[20];
    scan_state_t st = scan_state();

    btn_run.label = st == SCAN_STOPPED ? "Start" : st == SCAN_PAUSED ? "Resume" : "Pause";
    snprintf(stop_lbl, sizeof(stop_lbl), "Stop at\n%s", stops[stop_i].name);
    snprintf(speed_lbl, sizeof(speed_lbl), "Speed\n%s", speeds[speed_i].name);
    snprintf(hang_lbl, sizeof(hang_lbl), "Hang\n%d s", hangs_s[hang_i]);
    btn_stop.label  = stop_lbl;
    btn_speed.label = speed_lbl;
    btn_hang.label  = hang_lbl;
    buttons_refresh(&btn_run);
    buttons_refresh(&btn_stop);
    buttons_refresh(&btn_speed);
    buttons_refresh(&btn_hang);
}

static void configure(void) {
    scan_config(speeds[speed_i].ms, hangs_s[hang_i] * 1000, stops[stop_i].db, 10.0f);
}

/* ---- Running ---- */

static int64_t mono_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tune(int i) {
    if (i < 0 || i >= list_n) return;
    /* The frequency first, as the Channels app: a tune into another band loads that band's mode, which the
     * channel's then replaces. The mode first had the channel on air in the band's mode, and stored its own in the
     * band it left */
    cparam_i_set(cfg_fg_freq, list[i].freq);
    if (cparam_i_get(cfg_cur_mode) != list[i].mode) cparam_i_set(cfg_cur_mode, list[i].mode);
    tuned_freq = list[i].freq;
    tuned_seq  = meter_seq();
}

/* Start, Resume, Skip, Lock out and the MFK retune: not while the radio is on the air (the timer does not either) */
static bool on_air(void) {
    if (radio_get_state() == RADIO_RX) return false;
    msg_update_text_fmt("Scanner: not while transmitting");
    return true;
}

static void timer_cb(lv_timer_t *t) {
    static int   ticks;
    int64_t      now = mono_ms();
    scan_state_t was = scan_state();

    if (was != SCAN_STOPPED) {
        if (tuned_freq && cparam_i_get(cfg_fg_freq) != tuned_freq && was != SCAN_PAUSED) {
            scan_pause(true, now);      /* tuned by hand: stay there */
        } else if (radio_get_state() != RADIO_RX) {
            scan_touch(now);            /* on the air there: its hang from the end of the over, no retune */
        } else {
            static uint32_t seq;

            if (meter_seq() != seq) {     /* each reading once */
                seq = meter_seq();
                /* Not the first reading after a retune: a reading is the average of a frame, and that frame can
                 * still hold the frequency before (the X6200's meter is the base's own, with no settle frames).
                 * A strong one there stopped the scan on the next channel, and listed it there */
                if (seq - tuned_seq >= 2) scan_reading(now, meter_get_db());
            }
            tune(scan_tick(now, time(NULL)));
        }
    }

    scan_state_t st = scan_state();

    radio_set_scan_mute(st == SCAN_SEEKING);
    if (!backlight_seen()) return;      /* scanning goes on with the screen dark: only the drawing waits */
    if (st != was) labels_refresh();
    if (++ticks % 5 == 0 || st != was) fill();
}

static void start(void) {
    if (!list_n) {
        fill();
        return;
    }
    configure();
    /* From the channel the dial is on, else the top */
    int     from = 0;
    int32_t dial = cparam_i_get(cfg_fg_freq);

    for (int i = 0; i < list_n; i++)
        if (list[i].freq == dial) from = i;
    scan_start(from, mono_ms());
    tune(scan_tick(mono_ms(), time(NULL)));
}

static void run_cb(button_data_t *b) {
    switch (scan_state()) {
        case SCAN_STOPPED:
            if (on_air()) return;
            start();
            break;
        case SCAN_PAUSED:
            if (on_air()) return;
            /* Tuned at once, as start(): with the dial still where the knob left it the next tick paused it again,
             * and the tick after tuned the next one, paused */
            scan_pause(false, mono_ms());
            tune(scan_tick(mono_ms(), time(NULL)));
            break;
        default:
            scan_pause(true, mono_ms());
            break;
    }
    labels_refresh();
    fill();
}

static void source_cb(button_data_t *b) {
    for (int k = 0; k < SRC_N; k++) {
        source = (source + 1) % SRC_N;
        build_list();
        if (list_n) break;      /* the airband only where the radio receives it */
    }
    radio_set_scan_mute(false);
    labels_refresh();
    fill();
}

static void skip_cb(button_data_t *b) {
    if (on_air()) return;
    if (scan_state() == SCAN_PAUSED) scan_pause(false, mono_ms());
    else scan_skip(mono_ms());
    tune(scan_tick(mono_ms(), time(NULL)));     /* at once, as Resume */
    labels_refresh();
}

static void lock_cb(button_data_t *b) {
    int cur = scan_current();

    if (cur < 0 || on_air()) return;
    scan_lockout(cur, true);
    if (scan_state() != SCAN_STOPPED) {
        scan_skip(mono_ms());
        tune(scan_tick(mono_ms(), time(NULL)));
    }
    labels_refresh();
    fill();
}

static void stop_at_cb(button_data_t *b) {
    stop_i = (stop_i + 1) % (int)(sizeof(stops) / sizeof(stops[0]));
    configure();
    labels_refresh();
    fill();
}

static void speed_cb(button_data_t *b) {
    speed_i = (speed_i + 1) % (int)(sizeof(speeds) / sizeof(speeds[0]));
    configure();
    labels_refresh();
    fill();
}

static void hang_cb(button_data_t *b) {
    hang_i = (hang_i + 1) % (int)(sizeof(hangs_s) / sizeof(hangs_s[0]));
    configure();
    labels_refresh();
    fill();
}

static void clear_cb(button_data_t *b) {
    build_list();   /* levels, lockouts and what was heard: all afresh */
    radio_set_scan_mute(false);
    labels_refresh();
    fill();
}

/* By hand while paused or stopped: the next or the one before */
static void step(int d) {
    int cur = scan_current();

    if (!list_n || scan_state() == SCAN_SEEKING || scan_state() == SCAN_HOLD || on_air()) return;
    cur = cur < 0 ? 0 : (cur + d + list_n) % list_n;
    scan_start(cur, mono_ms());
    tune(scan_tick(mono_ms(), time(NULL)));
    scan_pause(true, mono_ms());
    labels_refresh();
    fill();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            step(-1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            step(1);
            break;
        case LV_KEY_ENTER:
            run_cb(NULL);
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_coord_t x, lv_coord_t y, lv_coord_t w) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    app_frame_create(&frame, dialog.obj);

    lv_obj_t *body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lbl_state = label(body, &sony_22, 12, 8, LEFT_W - 20);
    lbl_freq  = label(body, &sony_36, 12, 38, LEFT_W - 20);
    lbl_name  = label(body, &sony_22, 12, 84, LEFT_W - 20);

    meter_track = lv_obj_create(body);
    lv_obj_remove_style_all(meter_track);
    lv_obj_set_size(meter_track, LEFT_W - 30, 10);
    lv_obj_set_pos(meter_track, 12, 122);
    lv_obj_set_style_bg_color(meter_track, TC(track), 0);
    lv_obj_set_style_bg_opa(meter_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(meter_track, 3, 0);
    lv_obj_clear_flag(meter_track, LV_OBJ_FLAG_SCROLLABLE);
    meter_bar = lv_obj_create(meter_track);
    lv_obj_remove_style_all(meter_bar);
    lv_obj_set_size(meter_bar, 2, 10);
    lv_obj_set_style_bg_opa(meter_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(meter_bar, 3, 0);
    meter_thr = lv_obj_create(meter_track);
    lv_obj_remove_style_all(meter_thr);
    lv_obj_set_size(meter_thr, 3, 10);
    lv_obj_set_style_bg_color(meter_thr, TC(text_head), 0);
    lv_obj_set_style_bg_opa(meter_thr, LV_OPA_COVER, 0);

    lbl_info = label(body, &sony_18, 12, 142, LEFT_W - 20);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl_info, TC(text_dim), 0);

    hits_head = label(body, &sony_18, LEFT_W + 6, 6, APP_FRAME_W - LEFT_W - 14);
    lv_obj_set_style_text_color(hits_head, TC(text_dim), 0);
    lv_coord_t row = (APP_FRAME_BODY_H - 32) / HITS_SHOWN;
    for (int k = 0; k < HITS_SHOWN; k++)
        hit_rows[k] = label(body, &sony_18, LEFT_W + 6, 30 + k * row, APP_FRAME_W - LEFT_W - 14);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    channels_load();
    build_list();
    if (!list_n) source_cb(NULL);       /* no channels: the next list that has some */
    tuned_freq = 0;
    labels_refresh();
    fill();
    timer = lv_timer_create(timer_cb, TICK_MS, NULL);
}

static void destruct_cb(void) {
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    scan_stop();
    radio_set_scan_mute(false);
}

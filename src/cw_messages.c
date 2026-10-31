/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  CW keyer: memory texts sent with the morse key, once or as a beacon.
 *  Page 1: TX, New, Edit; page 2: Beacon, Delete, QRSS3; page 3: the
 *  beacon's start and end, the QRSS3 frequency. Same layout as the voice
 *  keyer.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_window.h"
#include "cw_messages.h"
#include "gui_styles.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "gui_events.h"
#include "key_input.h"
#include "text_popup.h"
#include "cw_encoder.h"
#include "msg_box.h"
#include "button_bar.h"
#include "screen_main.h"
#include "radio.h"
#include "theme.h"
#include "cw_beacon.h"

#define MAX_MSGS     64
#define TIMER_MS     100
#define CONFIRM_MS   3000

#define LIST_X       10
#define LIST_Y       40
#define LIST_W       470
#define LIST_H       (DIALOG_H - 86)
#define SIDE_X       494
#define SIDE_W       290

#define COLOR_HEAD   theme_hex(TC_text_head)
#define COLOR_DIM    theme_hex(TC_text_dim)
#define COLOR_SEL_BG theme_hex(TC_sel_bg)
#define COLOR_SEL    theme_hex(TC_sel_text)

typedef enum { EDIT_NONE, EDIT_NEW, EDIT_CHANGE } edit_t;
typedef enum { ADJ_NONE, ADJ_QRSS, ADJ_START, ADJ_UNTIL } adj_t;     /* what the MFK and VOL change on page 3 */

static uint32_t ids[MAX_MSGS];
static char    *texts[MAX_MSGS];
static int      count;
static int      sel;
static bool     beacon_on;
static bool     qrss_on;        /* TX and Beacon in QRSS3: not kept, after a restart they send at the keyer's speed */
static edit_t   editing;
static adj_t    adjusting;
static uint32_t delete_armed;

static lv_obj_t   *list, *head, *head_right, *state_lbl, *text_lbl, *info_lbl, *hint;
static lv_timer_t *timer;

static void window_open(lv_obj_t *parent);
static void window_close(void);
static void tx_btn_cb(button_data_t *btn);
static void new_cb(button_data_t *btn);
static void edit_cb(button_data_t *btn);
static void close_cb(button_data_t *btn);
static void beacon_cb(button_data_t *btn);
static void delete_cb(button_data_t *btn);
static void qrss_cb(button_data_t *btn);
static void qfreq_cb(button_data_t *btn);
static void start_cb(button_data_t *btn);
static void until_cb(button_data_t *btn);
static const char *tx_label(void);
static const char *beacon_label(void);
static const char *qrss_label(void);
static const char *qfreq_label(void);
static const char *start_label(void);
static const char *until_label(void);

static button_data_t btn_p1 = {.type = BTN_TEXT, .label = "(MSG 1:3)", .press = button_next_page_cb};
static button_data_t btn_tx = {.type = BTN_TEXT_FN, .label_fn = tx_label, .press = tx_btn_cb};
static button_data_t btn_new = {.type = BTN_TEXT, .label = "New", .press = new_cb};
static button_data_t btn_edit = {.type = BTN_TEXT, .label = "Edit", .press = edit_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static button_data_t btn_p2 = {.type = BTN_TEXT, .label = "(MSG 2:3)", .press = button_next_page_cb};
static button_data_t btn_beacon = {.type = BTN_TEXT_FN, .label_fn = beacon_label, .press = beacon_cb};
static button_data_t btn_delete = {.type = BTN_TEXT, .label = "Delete", .press = delete_cb};
static button_data_t btn_qrss = {.type = BTN_TEXT_FN, .label_fn = qrss_label, .press = qrss_cb};
static button_data_t btn_close2 = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static button_data_t btn_p3 = {.type = BTN_TEXT, .label = "(MSG 3:3)", .press = button_next_page_cb};
static button_data_t btn_start = {.type = BTN_TEXT_FN, .label_fn = start_label, .press = start_cb};
static button_data_t btn_until = {.type = BTN_TEXT_FN, .label_fn = until_label, .press = until_cb};
static button_data_t btn_qfreq = {.type = BTN_TEXT_FN, .label_fn = qfreq_label, .press = qfreq_cb};
static button_data_t btn_close3 = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

buttons_page_t buttons_page_msg_cw_1 = {{&btn_p1, &btn_tx, &btn_new, &btn_edit, &btn_close}};
buttons_page_t buttons_page_msg_cw_2 = {{&btn_p2, &btn_beacon, &btn_delete, &btn_qrss, &btn_close2}};
static buttons_page_t page_3 = {{&btn_p3, &btn_start, &btn_until, &btn_qfreq, &btn_close3}};

static dialog_t dialog = { .construct_cb = window_open, .destruct_cb = window_close, .btn_page = &buttons_page_msg_cw_1 };

dialog_t *dialog_msg_cw = &dialog;

/* Messages */

static void clear_msgs(void) {
    for (int i = 0; i < count; i++) free(texts[i]);
    count = 0;
}

/* Called by params_msg_cw_load() for each stored text */
void dialog_msg_cw_append(uint32_t id, const char *val) {
    if (count >= MAX_MSGS || !val) return;
    ids[count]   = id;
    texts[count] = strdup(val);
    count++;
}

static void load_msgs(int select) {
    clear_msgs();
    params_msg_cw_load();

    lv_table_set_row_cnt(list, count > 0 ? count : 1);
    if (count == 0) lv_table_set_cell_value(list, 0, 0, "No messages yet: press New");
    for (int i = 0; i < count; i++) lv_table_set_cell_value(list, i, 0, texts[i]);

    sel = select < 0 ? count - 1 : select;
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    lv_label_set_text_fmt(head_right, "%i message%s", count, count == 1 ? "" : "s");
    lv_obj_invalidate(list);
}

static const char *sel_text(void) {
    return count > 0 ? texts[sel] : NULL;
}

/* Seconds to key a text as sent ({CALL} filled in): PARIS timing, 50 dits a word; in QRSS3 its elements */
static float text_secs(const char *text) {
    text = cw_encoder_expand(text);
    if (qrss_on) {
        int      n  = qrss3_plan(text, NULL, 0);
        cw_el_t *el = n ? malloc(n * sizeof(*el)) : NULL;
        float    s  = 0.0f;

        if (el) {
            s = cw_plan_ms(el, qrss3_plan(text, el, n)) / 1000.0f;
            free(el);
        }
        return s;
    }
    float words = strlen(text) / 5.0f;
    return words * 60.0f / param_i_get(cfg_key_speed);
}

/* QRSS3: the band of qrss_bands the radio is on (-1 none), the frequency chosen there */
static int qrss_band(void) {
    return qrss_band_of(cparam_i_get(cfg_fg_freq));
}

static int32_t qrss_hz(int band) {
    return qrss_freq(band, params.cw_qrss_hz[band].x);
}

/* The beacon's schedule as set (page 3; kept like the pause), what the settings hold checked */
static bcn_sched_t sched_set(void) {
    bcn_sched_t s = {params.cw_bcn_start.x, params.cw_bcn_until.x, params.cw_bcn_sends.x, params.cw_bcn_end.x};

    if (s.start_min < BCN_NOW || s.start_min >= 24 * 60) s.start_min = BCN_NOW;
    if (s.until > BCN_UNTIL_TIME) s.until = BCN_UNTIL_STOP;
    if (s.sends < 1 || s.sends > BCN_SENDS_MAX) s.sends = 10;
    if (s.end_min < 0 || s.end_min >= 24 * 60) s.end_min = BCN_NOW;
    if (s.until == BCN_UNTIL_TIME && s.end_min < 0) s.until = BCN_UNTIL_STOP;
    return s;
}

/* The screen clock's minute now, and its time zone's name (UTC unless Settings > General > DATE / TIME has one) */
static int now_min(void) {
    return bcn_min_of(time(NULL));
}

static const char *zone(void) {
    static char buf[16];
    time_t      now = time(NULL);
    struct tm   tm;

    localtime_r(&now, &tm);
    if (!strftime(buf, sizeof(buf), "%Z", &tm)) buf[0] = 0;
    return buf;
}

/* "From 20:00 UTC, 10 sends", "From 20:00 to 21:00 UTC", "Now, until stopped" */
static const char *sched_words(void) {
    static char buf[48];
    char        until[24];
    bcn_sched_t s = sched_set();

    if (s.until == BCN_UNTIL_TIME && s.start_min < 0)
        snprintf(buf, sizeof(buf), "Now, until %02i:%02i %s", s.end_min / 60, s.end_min % 60, zone());
    else if (s.until == BCN_UNTIL_TIME)
        snprintf(buf, sizeof(buf), "From %02i:%02i to %02i:%02i %s", s.start_min / 60, s.start_min % 60,
                 s.end_min / 60, s.end_min % 60, zone());
    else {
        if (s.until == BCN_UNTIL_SENDS) snprintf(until, sizeof(until), "%i send%s", s.sends, s.sends == 1 ? "" : "s");
        else snprintf(until, sizeof(until), "until stopped");
        if (s.start_min < 0) snprintf(buf, sizeof(buf), "Now, %s", until);
        else snprintf(buf, sizeof(buf), "From %02i:%02i %s, %s", s.start_min / 60, s.start_min % 60, zone(), until);
    }
    return buf;
}

/* 10.140050: into buf of 16 */
static const char *mhz(char *buf, int32_t hz) {
    snprintf(buf, 16, "%d.%06d", (int)(hz / 1000000), (int)(hz % 1000000));
    return buf;
}

/* Buttons */

static bool sending(void) {
    return cw_encoder_state() != CW_ENCODER_IDLE;
}

static const char *tx_label(void) {
    return sending() ? "Stop" : "TX";
}

static const char *beacon_label(void) {
    static char buf[24];
    if (!beacon_on) return "Beacon\nOff";
    snprintf(buf, sizeof(buf), "Beacon\n%i s", params.cw_encoder_period);
    return buf;
}

static const char *qrss_label(void) {
    return qrss_on ? "QRSS3\nOn" : "QRSS3\nOff";
}

static const char *start_label(void) {
    static char buf[24];
    bcn_sched_t s = sched_set();

    if (s.start_min < 0) return "Start\nNow";
    snprintf(buf, sizeof(buf), "Start\n%02i:%02i", s.start_min / 60, s.start_min % 60);
    return buf;
}

static const char *until_label(void) {
    static char buf[24];
    bcn_sched_t s = sched_set();

    if (s.until == BCN_UNTIL_SENDS)
        snprintf(buf, sizeof(buf), "Until\n%i send%s", s.sends, s.sends == 1 ? "" : "s");
    else if (s.until == BCN_UNTIL_TIME)
        snprintf(buf, sizeof(buf), "Until\n%02i:%02i", s.end_min / 60, s.end_min % 60);
    else
        return "Until\nStopped";
    return buf;
}

static const char *qfreq_label(void) {
    static char buf[32];
    char        f[16];
    int         band = qrss_band();

    if (band < 0) return "Freq\n30/80 m";
    snprintf(buf, sizeof(buf), "Freq\n%s", mhz(f, qrss_hz(band)));
    return buf;
}

static void set_btn(button_data_t *b, bool disabled) {
    if (b->disabled != disabled) buttons_disabled(b, disabled);
}

/* What the MFK and VOL change on page 3: its button marked meanwhile */
static void set_adjust(adj_t a) {
    adjusting = a;
    if (btn_qfreq.mark != (a == ADJ_QRSS)) buttons_mark(&btn_qfreq, a == ADJ_QRSS);
    if (btn_start.mark != (a == ADJ_START)) buttons_mark(&btn_start, a == ADJ_START);
    if (btn_until.mark != (a == ADJ_UNTIL)) buttons_mark(&btn_until, a == ADJ_UNTIL);
}

static void update_buttons(void) {
    bool busy  = sending();
    bool empty = count == 0;

    set_btn(&btn_tx, !busy && empty);
    set_btn(&btn_new, busy);
    set_btn(&btn_edit, busy || empty);
    set_btn(&btn_beacon, busy);
    set_btn(&btn_delete, busy || empty);
    set_btn(&btn_qrss, busy);
    set_btn(&btn_qfreq, busy);
    set_btn(&btn_start, busy);
    set_btn(&btn_until, busy);
    set_btn(&btn_p1, busy);
    set_btn(&btn_p2, busy);
    set_btn(&btn_p3, busy);

    static const char *tx_l;
    const char        *now = tx_label();
    if (now != tx_l) {
        tx_l = now;
        if (btn_tx.disp_btn) buttons_refresh(&btn_tx);
    }
    /* the band's frequency, or none, as the radio moves */
    static char qf_l[32];
    if (strcmp(qf_l, qfreq_label())) {
        snprintf(qf_l, sizeof(qf_l), "%s", qfreq_label());
        if (btn_qfreq.disp_btn) buttons_refresh(&btn_qfreq);
    }
}

/* Status panel */

static void set_state(const char *text, uint32_t bg, uint32_t fg) {
    lv_label_set_text(state_lbl, text);
    lv_obj_set_style_bg_color(state_lbl, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(fg), 0);
}

static void update_status(void) {
    const char *text = sel_text();
    int         wpm  = param_i_get(cfg_key_speed);
    int         band = qrss_band();
    char        f[16], lo[16], hi[16];

    if (!qrss_on)
        lv_label_set_text_fmt(head, "CW KEYER - #%06x %i WPM#", theme_hex(TC_text), wpm);
    else if (band >= 0)
        lv_label_set_text_fmt(head, "CW KEYER - #%06x QRSS3 %s MHz#", theme_hex(TC_text), mhz(f, qrss_hz(band)));
    else
        lv_label_set_text_fmt(head, "CW KEYER - #%06x QRSS3: 30 m or 80 m#", theme_hex(TC_text));
    lv_label_set_text(text_lbl, text ? text : "-");

    switch (cw_encoder_state()) {
        case CW_ENCODER_SEND:
        case CW_ENCODER_BEACON:
            set_state("ON AIR", theme_hex(TC_tx_bg), theme_hex(TC_text));
            if (cw_encoder_qrss3()) {
                int left = cw_encoder_send_left();
                lv_label_set_text_fmt(hint, "TX or PTT: stop - %i:%02i to go", left / 60, left % 60);
            } else {
                lv_label_set_text(hint, "TX or PTT: stop");
            }
            break;
        case CW_ENCODER_BEACON_IDLE: {
            char        buf[32];
            bcn_sched_t s = sched_set();

            snprintf(buf, sizeof(buf), "BEACON %i s", cw_encoder_pause_left());
            set_state(buf, theme_hex(TC_chip), theme_hex(TC_warn));
            if (s.until == BCN_UNTIL_SENDS)
                lv_label_set_text_fmt(hint, "%i of %i sent - TX or PTT: stop", cw_encoder_sends_done(), s.sends);
            else
                lv_label_set_text_fmt(hint, "%i sent - TX or PTT: stop", cw_encoder_sends_done());
            break;
        }
        case CW_ENCODER_BEACON_WAIT: {
            char        buf[32];
            bcn_sched_t s    = sched_set();
            int         left = cw_encoder_wait_left();

            snprintf(buf, sizeof(buf), "AT %02i:%02i", s.start_min / 60 % 24, s.start_min % 60);
            set_state(buf, theme_hex(TC_chip), theme_hex(TC_warn));
            lv_label_set_text_fmt(hint, "Starts in %i:%02i:%02i - TX or PTT: stop", left / 3600, left / 60 % 60,
                                  left % 60);
            break;
        }
        default:
            set_state("READY", theme_hex(TC_chip), theme_hex(TC_text_head));
            if (adjusting == ADJ_QRSS)
                lv_label_set_text(hint, "MFK or VOL: 10 Hz steps - press MFK: done");
            else if (adjusting == ADJ_START || (adjusting == ADJ_UNTIL && sched_set().until == BCN_UNTIL_TIME))
                lv_label_set_text_fmt(hint, "MFK or VOL: 5 min steps (%s clock) - press MFK: done", zone());
            else if (adjusting == ADJ_UNTIL)
                lv_label_set_text(hint, "MFK or VOL: how many - Until: next kind - MFK: done");
            else
                lv_label_set_text(hint, count ? "MFK: choose - press MFK: edit" : "New: write the first message");
            break;
    }

    if (text && qrss_on) {
        float secs = text_secs(text);
        char  win[48] = "On 30 m or 80 m only";

        if (beacon_on)
            snprintf(win, sizeof(win), "%s", sched_words());
        else if (band >= 0)
            snprintf(win, sizeof(win), "Window %s-%s", mhz(lo, qrss_bands[band].win_lo_hz),
                     mhz(hi, qrss_bands[band].win_hi_hz));
        lv_label_set_text_fmt(info_lbl, secs < 120.0f ? "About %.0f s in QRSS3\nBeacon: %s\n%s"
                                                      : "About %.0f min in QRSS3\nBeacon: %s\n%s",
                              secs < 120.0f ? secs : secs / 60.0f, beacon_on ? "on" : "off (page 2)", win);
    } else if (text) {
        lv_label_set_text_fmt(info_lbl, "About %.0f s at %i WPM\nBeacon: %s%s%s", text_secs(text), wpm,
                              beacon_on ? "on" : "off (page 2)", beacon_on ? "\n" : "", beacon_on ? sched_words() : "");
    } else {
        lv_label_set_text(info_lbl, "");
    }
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    if (delete_armed && lv_tick_elaps(delete_armed) > CONFIRM_MS) delete_armed = 0;
    if (adjusting && (buttons_get_cur_page() != &page_3 || sending() || (adjusting == ADJ_QRSS && qrss_band() < 0)))
        set_adjust(ADJ_NONE);
    update_status();
    update_buttons();
}

/* List */

static void list_draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS) return;
    uint32_t r = dsc->id;

    dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
    dsc->rect_dsc->bg_color = TC(panel);
    if (count == 0) {
        dsc->label_dsc->color = lv_color_hex(COLOR_DIM);
        return;
    }
    if ((int)r == sel) {
        dsc->rect_dsc->bg_color     = lv_color_hex(COLOR_SEL_BG);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 2;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM;
        dsc->label_dsc->color       = lv_color_hex(COLOR_SEL);
    }
}

static void move_sel(int step) {
    if (sending() || count == 0) return;
    sel += step;
    if (sel < 0) sel = 0;
    if (sel >= count) sel = count - 1;

    lv_table_t *t = (lv_table_t *)list;
    lv_coord_t  y = 0;
    for (int i = 0; i < sel; i++) y += t->row_h[i];
    lv_obj_scroll_to_y(list, y + t->row_h[sel] / 2 - LIST_H / 2, LV_ANIM_OFF);
    lv_obj_invalidate(list);
    delete_armed = 0;
}

/* Keys */

/* Page 3's value, step by step */
static void adjust(int step) {
    int         band = qrss_band();
    bcn_sched_t s    = sched_set();

    if (adjusting == ADJ_QRSS && band >= 0) {
        params_int32_set(&params.cw_qrss_hz[band], qrss_freq_step(band, params.cw_qrss_hz[band].x, step));
    } else if (adjusting == ADJ_START) {
        params_int32_set(&params.cw_bcn_start, bcn_step_start(s.start_min, step, now_min()));
    } else if (adjusting == ADJ_UNTIL && s.until == BCN_UNTIL_SENDS) {
        params_uint16_set(&params.cw_bcn_sends, bcn_step_sends(s.sends, step));
    } else if (adjusting == ADJ_UNTIL && s.until == BCN_UNTIL_TIME) {
        params_int32_set(&params.cw_bcn_end, bcn_step_end(s.end_min, step, s.start_min < 0 ? now_min() : s.start_min));
    }
    buttons_refresh(&btn_qfreq);
    buttons_refresh(&btn_start);
    buttons_refresh(&btn_until);
    timer_cb(NULL);
}

static void adjust_done(void) {
    set_adjust(ADJ_NONE);
    timer_cb(NULL);
}

/* Arrows move in the list, ENTER or F4 edits, ESC closes, VOL keeps the volume. While page 3 adjusts its value the
 * MFK, the arrows and VOL change it, ENTER is done */
static void key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (adjusting && key != LV_KEY_ESC) {
        if (key == LV_KEY_LEFT || key == LV_KEY_DOWN || key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
            adjust(-1);
        else if (key == LV_KEY_RIGHT || key == LV_KEY_UP || key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
            adjust(1);
        else if (key == LV_KEY_ENTER)
            adjust_done();
        return;
    }
    if (key == LV_KEY_LEFT || key == LV_KEY_UP)
        move_sel(-1);
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN)
        move_sel(1);
    else if (key == LV_KEY_ENTER || key == KEYBOARD_F4)
        edit_cb(NULL);
    else if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}

/* MFK press: the encoder sends a click, not ENTER. */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;
    if (adjusting) adjust_done();
    else edit_cb(NULL);
}

/* PTT stops a beacon during its pause, and before its start time. */
static void radio_tx_cb(lv_event_t *e) {
    (void)e;
    if (cw_encoder_state() == CW_ENCODER_BEACON_IDLE || cw_encoder_state() == CW_ENCODER_BEACON_WAIT) cw_encoder_stop();
}

/* Text editing */

static void edit_end(void) {
    textarea_window_close();
    editing = EDIT_NONE;
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static bool edit_ok(void) {
    const char *val = textarea_window_get();
    if (!val || !*val) {
        msg_update_text_fmt("Empty message");
        return false;
    }
    edit_t what = editing;
    if (what == EDIT_NEW) params_msg_cw_new(val);
    else params_msg_cw_edit(ids[sel], val);
    int keep = what == EDIT_NEW ? -1 : sel;
    edit_end();
    load_msgs(keep);
    return true;
}

static bool edit_cancel(void) {
    edit_end();
    return true;
}

static void open_editor(edit_t what) {
    if (sending() || editing != EDIT_NONE) return;
    if (what == EDIT_CHANGE && !sel_text()) return;

    editing = what;
    /* The text area gets the keys only when the dialog is out of the group. */
    lv_group_remove_obj(dialog.obj);
    textarea_window_open(edit_ok, edit_cancel);
    if (what == EDIT_CHANGE) textarea_window_set(sel_text());
}

/* Button actions */

/* QRSS3 where the radio is: CW or CW-R on 30 m or 80 m, moved onto the band's QRSS frequency, which it must transmit
 * on (no split, no TX shift) */
static void qrss_start(const char *text) {
    radio_mode_t mode = cparam_i_get(cfg_cur_mode);
    int          band = qrss_band();
    int32_t      hz;
    char         f[16];

    if (mode != radio_mode_cw && mode != radio_mode_cwr) {
        msg_update_text_fmt("QRSS3: in CW or CW-R only");
        return;
    }
    if (band < 0) {
        msg_update_text_fmt("QRSS3: on 30 m or 80 m only");
        return;
    }
    if (param_i_get(cfg_band_split)) {      /* before the move: the carrier would go out on the other VFO */
        msg_update_text_fmt("QRSS3: split or TX shift on");
        return;
    }
    hz = qrss_hz(band);
    if (cparam_i_get(cfg_fg_freq) != hz) {
        if (main_screen_freq_locked()) {
            msg_update_text_fmt("QRSS3: frequency locked");
            return;
        }
        main_screen_set_freq(hz);
    }
    if (radio_tx_freq() != hz) {
        msg_update_text_fmt("QRSS3: split or TX shift on");
        return;
    }
    if (!cfg_is_tx_freq(hz)) {
        msg_update_text_fmt("No TX on %s MHz: receive only", mhz(f, hz));
        return;
    }
    if (beacon_on) {
        bcn_sched_t s = sched_set();
        cw_encoder_schedule(&s);
    }
    if (!cw_encoder_send_qrss3(text, beacon_on)) msg_update_text_fmt("QRSS3: not started");
}

static void tx_btn_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) cw_encoder_stop();
    else if (sel_text() && qrss_on) qrss_start(cw_encoder_expand(sel_text()));
    else if (sel_text()) {
        if (beacon_on) {
            bcn_sched_t s = sched_set();
            cw_encoder_schedule(&s);
        }
        cw_encoder_send(cw_encoder_expand(sel_text()), beacon_on);
    }
    timer_cb(NULL);
}

static void new_cb(button_data_t *btn) {
    (void)btn;
    open_editor(EDIT_NEW);
}

static void edit_cb(button_data_t *btn) {
    (void)btn;
    open_editor(EDIT_CHANGE);
}

static void close_cb(button_data_t *btn) {
    (void)btn;
    dialog_destruct();
}

/* Off, then the pause after each sending: past the last one back to off */
static const int beacon_pauses[] = { 10, 30, 60, 120 };

/* The first longer pause after the starting one; 0 past the last */
static int next_pause(int now) {
    for (size_t i = 1; i <sizeof(beacon_pauses) / sizeof(beacon_pauses[0]); i++)
        if (beacon_pauses[i] > now) return beacon_pauses[i];
    return 0;
}

static void beacon_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) return;

    params_lock();
    int pause = beacon_on ? next_pause(params.cw_encoder_period) : beacon_pauses[0];

    beacon_on = pause != 0;
    if (beacon_on) params.cw_encoder_period = pause;
    params_unlock(&params.dirty.cw_encoder_period);
    buttons_refresh(&btn_beacon);
    timer_cb(NULL);
}

static void qrss_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) return;
    qrss_on = !qrss_on;
    buttons_refresh(&btn_qrss);
    timer_cb(NULL);
}

/* The beacon's start: the MFK changes it while the button is marked */
static void start_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) return;
    set_adjust(adjusting == ADJ_START ? ADJ_NONE : ADJ_START);
    timer_cb(NULL);
}

/* The beacon's end: until stopped, after a number of sends, at a time; the MFK changes the number or the time */
static void until_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) return;

    bcn_sched_t s    = sched_set();
    int         next = (s.until + 1) % (BCN_UNTIL_TIME + 1);

    if (next == BCN_UNTIL_TIME && s.end_min < 0)    /* an hour on, about */
        params_int32_set(&params.cw_bcn_end, bcn_step_end(BCN_NOW, 11, s.start_min < 0 ? now_min() : s.start_min));
    params_uint8_set(&params.cw_bcn_until, next);
    set_adjust(next == BCN_UNTIL_STOP ? ADJ_NONE : ADJ_UNTIL);
    buttons_refresh(&btn_until);
    timer_cb(NULL);
}

/* The QRSS3 frequency of the band the radio is on: the MFK changes it while the button is marked */
static void qfreq_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) return;
    if (qrss_band() < 0) {
        msg_update_text_fmt("QRSS3: on 30 m or 80 m only");
        return;
    }
    set_adjust(adjusting == ADJ_QRSS ? ADJ_NONE : ADJ_QRSS);
    timer_cb(NULL);
}

static void delete_cb(button_data_t *btn) {
    (void)btn;
    if (sending() || !sel_text()) return;

    if (!delete_armed) {
        delete_armed = lv_tick_get();
        msg_update_text_fmt("Press Delete again to delete the message");
        return;
    }
    params_msg_cw_delete(ids[sel]);
    delete_armed = 0;
    msg_update_text_fmt("Message deleted");
    load_msgs(sel);
    timer_cb(NULL);
}

/* Dialog */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void window_open(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    btn_p1.next = &buttons_page_msg_cw_2;
    btn_p2.next = &page_3;
    btn_p3.next = &buttons_page_msg_cw_1;
    btn_p1.prev = &page_3;
    btn_p2.prev = &buttons_page_msg_cw_1;
    btn_p3.prev = &buttons_page_msg_cw_2;

    head = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_label_set_recolor(head, true);
    lv_obj_set_pos(head, 16, 10);
    head_right = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_obj_set_width(head_right, 250);
    lv_obj_set_style_text_align(head_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(head_right, 796 - 16 - 250, 10);

    list = lv_table_create(dialog.obj);
    lv_obj_remove_style(list, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(list, LIST_W, LIST_H);
    lv_obj_set_pos(list, LIST_X, LIST_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, theme_shape.border, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, TC(panel_border), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(list, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(list, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(list, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(list, 10, LV_PART_ITEMS);
    lv_table_set_col_cnt(list, 1);
    lv_table_set_col_width(list, 0, LIST_W - 2);
    lv_obj_add_event_cb(list, list_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    lv_obj_t *side = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, SIDE_W, LIST_H);
    lv_obj_set_pos(side, SIDE_X, LIST_Y);
    lv_obj_set_style_bg_color(side, TC(side), 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(side, TC(panel_border), 0);
    lv_obj_set_style_border_width(side, theme_shape.border, 0);
    lv_obj_set_style_radius(side, 8, 0);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    state_lbl = label(side, &sony_28, COLOR_HEAD);
    lv_obj_set_size(state_lbl, SIDE_W - 24, 48);
    lv_obj_set_pos(state_lbl, 12, 12);
    lv_obj_set_style_bg_opa(state_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(state_lbl, 8, 0);
    lv_obj_set_style_text_align(state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(state_lbl, 8, 0);

    /* Three lines of the text at most, three of information under it: at 196 the panel cut the information after
     * its first line */
    text_lbl = label(side, &sony_20, theme_hex(TC_text_value));
    lv_obj_set_size(text_lbl, SIDE_W - 24, 62);
    lv_label_set_long_mode(text_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(text_lbl, 12, 72);

    info_lbl = label(side, &sony_18, COLOR_DIM);
    lv_obj_set_width(info_lbl, SIDE_W - 24);
    lv_obj_set_pos(info_lbl, 12, 142);

    hint = label(dialog.obj, &sony_18, COLOR_DIM);
    lv_obj_set_pos(hint, 16, DIALOG_H - 32);

    lv_obj_add_event_cb(dialog.obj, radio_tx_cb, EVENT_RADIO_TX, NULL);
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    load_msgs(0);
    main_screen_lock_mode(true);
    timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
    timer_cb(NULL);
}

static void window_close(void) {
    cw_encoder_stop();
    set_adjust(ADJ_NONE);
    if (editing != EDIT_NONE) {
        textarea_window_close();
        editing = EDIT_NONE;
    }
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    lv_group_remove_obj(dialog.obj);
    clear_msgs();
    delete_armed = 0;
    main_screen_lock_mode(false);
}

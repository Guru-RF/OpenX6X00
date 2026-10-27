/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "tx_meters.h"

#include <stdio.h>

#include "gui_events.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "scheduler.h"
#include "gui_styles.h"
#include "theme.h"
#include "util.h"
#include "info_bar.h"
#include "radio.h"             /* radio_tx_freq */
#include "rtty.h"
#include "ft8_window.h"
#include "dialog_js8.h"
#include "dialog_psk.h"
#include "dialog_olivia.h"
#include "dialog_jtty.h"
#include "dialog_wspr.h"
#include "app_sstv.h"

/* The bar scales: PWR 0-10 W, SWR 1-5 (more shows 5), ALC 0-10 */
#define PWR_MAX     10.0f
#define SWR_MIN     1.0f
#define SWR_MAX     5.0f
#define ALC_MAX     10.0f

/* The readings of this TX: smoothed, except in the digital modes where each tone counts */
static float         pwr, vswr, alc;
static uint8_t       msg_id;          /* one more with each reading (tx_info_refresh()) */
static radio_mode_t  cur_mode;        /* followed from the first mode change on */

static lv_obj_t     *obj;        /* "TX" in the S-meter box */
static lv_obj_t     *panel;      /* big TX panel under the top bars */
static lv_obj_t     *head_label;
static lv_obj_t     *time_label;
static lv_obj_t     *pwr_value;
static lv_obj_t     *swr_value;
static lv_obj_t     *alc_value;
static lv_obj_t     *alc_name;
static uint32_t      tx_start;
static float         tx_len;         /* tx_info_set_length */

/* Digital apps: a strip under the frequency bar instead of the panel */
static lv_obj_t     *strip;
static lv_obj_t     *strip_fill;
static lv_obj_t     *strip_pwr;
static lv_obj_t     *strip_swr;
static lv_obj_t     *strip_time;

#define STRIP_BAR_W 260

#define PANEL_W   500
#define PANEL_H   190
#define ROW_Y     52
#define ROW_STEP  44
#define BARS_X    72
#define BARS_W    275
#define SLICES    25

static void mode_changed(Subject *subj, void *user_data) {
    cur_mode = cparam_i_get(cfg_cur_mode);
}

/* CW: the base makes the carrier itself at a fixed level and its ALC turns it down to the power
 * set (dummy load, 2026-10-17: 1 W ALC 5.5, 5 W 4.9, power right), so the ALC only shows how far
 * down; nothing for the operator to set, shown greyed */
static bool alc_greyed(void) {
    return cur_mode == radio_mode_cw || cur_mode == radio_mode_cwr;
}

/* One bar row: SLICES slices, lit up to val; SWR turns yellow then red. */
static void draw_bar(lv_draw_ctx_t *draw_ctx, lv_coord_t x1, lv_coord_t y1, float min, float max, float val,
                     bool swr, uint32_t on_color) {
    lv_draw_rect_dsc_t rect_dsc;
    lv_area_t          area;
    lv_coord_t         slice_w = BARS_W / SLICES;
    int                lit = (LV_MAX(val, min) - min) / (max - min) * SLICES + 0.5f;

    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_COVER;
    area.y1 = y1;
    area.y2 = y1 + 19;

    for (int i = 0; i < SLICES; i++) {
        float v = min + (max - min) * i / SLICES;

        if (i >= lit) {
            rect_dsc.bg_color = TC(chip);
        } else if (swr && v >= 3.0f) {
            rect_dsc.bg_color = TC(bar_high);
        } else if (swr && v >= 2.0f) {
            rect_dsc.bg_color = TC(bar_mid);
        } else {
            rect_dsc.bg_color = lv_color_hex(on_color);
        }
        /* 1bit: the unlit slices hollow (chip is the paper), so the scale stays in sight */
        rect_dsc.border_width = theme_shape.mono && i >= lit ? 1 : 0;
        rect_dsc.border_color = TC(text);
        area.x1 = x1 + i * slice_w;
        area.x2 = area.x1 + slice_w - 3;
        lv_draw_rect(draw_ctx, &rect_dsc, &area);
    }
}

static void panel_draw_cb(lv_event_t *e) {
    lv_obj_t      *o        = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_coord_t     x1       = o->coords.x1 + BARS_X;
    lv_coord_t     y1       = o->coords.y1 + ROW_Y + 4;

    draw_bar(draw_ctx, x1, y1, 0.0f, PWR_MAX, pwr, false, theme_hex(TC_line));
    /* SWR in its own colour (web Display page), else the theme's; after dl2zw's SWR colour, OpenX6100 */
    int32_t swr_own = param_i_get(cfg_swr_color);
    draw_bar(draw_ctx, x1, y1 + ROW_STEP, SWR_MIN, SWR_MAX, vswr, true, swr_own < 0 ? theme_hex(TC_line) : swr_own);
    draw_bar(draw_ctx, x1, y1 + 2 * ROW_STEP, 0.0f, ALC_MAX, alc, false,
             alc_greyed() ? theme_hex(TC_text_mute) : theme_hex(TC_alc));

    /* PWR scale under its bar */
    lv_draw_label_dsc_t label_dsc;
    lv_area_t           area;
    char                text[4];

    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = TC(text_dim);
    label_dsc.font  = &sony_18;
    for (int w = 0; w <= 10; w += 2) {
        snprintf(text, sizeof(text), "%i", w);
        area.x1 = x1 + BARS_W * w / 10 - 8;
        area.x2 = area.x1 + 26;
        area.y1 = y1 + 22;
        area.y2 = area.y1 + 20;
        lv_draw_label(draw_ctx, &label_dsc, &area, text, NULL);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void update_head(void) {
    uint32_t  s    = lv_tick_elaps(tx_start) / 1000;
    int32_t   f    = radio_tx_freq();      /* in split the other VFO's, or the TX shift's */
    uint16_t  mhz, khz, hz;

    split_freq(f, &mhz, &khz, &hz);
    lv_label_set_text_fmt(head_label, "%s - %i.%03i.%03i", info_params_mode_label_get(), mhz, khz, hz);
    lv_label_set_text_fmt(time_label, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void update_strip(void) {
    uint32_t s   = lv_tick_elaps(tx_start) / 1000;
    float    ref = LV_MAX(param_f_get(cfg_pwr), 0.1f);
    /* SWR from 2 yellow, from 3 red (as the panel's bar and the walkie) */
    lv_color_t c = vswr >= 3.0f ? TC(bar_high) : vswr >= 2.0f ? TC(bar_mid) : lv_color_white();

    lv_obj_set_width(strip_fill, STRIP_BAR_W * LV_MIN(pwr / ref, 1.0f));
    lv_label_set_text_fmt(strip_pwr, "%.1f W", pwr);
    lv_label_set_text_fmt(strip_swr, "%.1f", vswr);
    lv_obj_set_style_text_color(strip_swr, c, 0);
    lv_obj_set_style_border_color(strip, vswr >= 2.0f ? c : lv_color_make(230, 50, 50), 0);
    if (tx_len > 0.0f)
        lv_label_set_text_fmt(strip_time, "%u / %u s", (unsigned)s, (unsigned)(tx_len + 0.5f));
    else
        lv_label_set_text_fmt(strip_time, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void tx_info_set_length(float seconds) {
    tx_len = seconds;
}

/* The digital apps keep their screen while transmitting: the strip, not the panel */
static bool digital_app_open(void) {
    static dialog_t **const apps[] = {&dialog_ft8, &dialog_js8, &dialog_wspr, &dialog_sstv,
                                      &dialog_psk, &dialog_olivia, &dialog_jtty};

    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++)
        if (dialog_type_is_run(*apps[i])) return true;
    return rtty_get_state() != RTTY_OFF;
}

/* TX: the readings from zero, the strip or the panel (under the frequency strip). RX: all hidden */
static void radio_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == EVENT_RADIO_RX) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    pwr = vswr = alc = 0.0f;
    tx_start = lv_tick_get();

    if (digital_app_open()) {
        update_strip();
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_y(panel, DIALOG_Y + 8 + METER_ROW_H);   /* VISION: under the S-meter, framed red for TX */
    update_head();
    lv_obj_set_style_text_color(alc_name, alc_greyed() ? TC(text_mute) : TC(text_value), 0);
    lv_obj_set_style_text_color(alc_value, alc_greyed() ? TC(text_mute) : TC(text), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel);
}

/* In the LVGL thread: the new readings on whichever view is shown */
static void update_tx_info(void *unused) {
    if (!lv_obj_has_flag(strip, LV_OBJ_FLAG_HIDDEN)) {
        update_strip();
    } else if (!lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text_fmt(pwr_value, "%.1f W", pwr);
        lv_label_set_text_fmt(swr_value, "%.1f", vswr);
        lv_label_set_text_fmt(alc_value, "%.1f", alc);
        update_head();
        lv_obj_invalidate(panel);
    }
}

lv_obj_t *tx_info_init(lv_obj_t *parent) {
    /* Carries the TX/RX events (the S-meter stays on screen, framed red: meter.c) */
    obj = lv_obj_create(parent);
    lv_obj_add_style(obj, &tx_info_style, 0);
    /* Solid: LVGL caches gradients by size, the S-meter's would be reused */
    lv_obj_set_style_bg_color(obj, TC(tx_box), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_color(obj, TC(err), 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *tx = make_label(obj, &sony_30, theme_hex(TC_text));
    lv_label_set_text(tx, "TX");
    lv_obj_center(tx);

    /* The big panel */
    panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, PANEL_W, PANEL_H);
    lv_obj_set_x(panel, (800 - PANEL_W) / 2);
    lv_obj_set_style_bg_color(panel, TC(popup), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, TC(err), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, theme_shape.radius_popup, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(panel, panel_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    lv_obj_t *badge = make_label(panel, &sony_22, theme_hex(TC_text));
    lv_label_set_text(badge, "TX");
    lv_obj_set_style_bg_color(badge, TC(tx_bg), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 5, 0);
    lv_obj_set_style_pad_hor(badge, 10, 0);
    lv_obj_set_style_pad_ver(badge, 2, 0);
    lv_obj_set_pos(badge, 16, 7);

    head_label = make_label(panel, &sony_22, theme_hex(TC_text_head));
    lv_obj_align(head_label, LV_ALIGN_TOP_MID, 0, 9);
    time_label = make_label(panel, &sony_22, theme_hex(TC_text));
    lv_obj_align(time_label, LV_ALIGN_TOP_RIGHT, -16, 9);

    lv_obj_t *line = lv_obj_create(panel);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, PANEL_W - 36, 1);
    lv_obj_set_pos(line, 16, 42);
    lv_obj_set_style_bg_color(line, TC(panel_border), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    static const char *names[] = {"PWR", "SWR", "ALC"};
    lv_obj_t          *values[3];
    for (int i = 0; i < 3; i++) {
        lv_obj_t *n = alc_name = make_label(panel, &sony_22, theme_hex(TC_text_value));   /* the last: ALC */
        lv_label_set_text(n, names[i]);
        lv_obj_set_pos(n, 16, ROW_Y + i * ROW_STEP);
        values[i] = make_label(panel, &sony_28, theme_hex(TC_text));
        lv_obj_set_width(values[i], 110);
        lv_obj_set_style_text_align(values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(values[i], PANEL_W - 16 - 4 - 110, ROW_Y - 4 + i * ROW_STEP);
    }
    pwr_value = values[0];
    swr_value = values[1];
    alc_value = values[2];

    /* The strip, above the open app (again on a theme change: the old one goes) */
    if (strip) lv_obj_del(strip);
    strip = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(strip);
    lv_obj_set_pos(strip, 16, 121);
    lv_obj_set_size(strip, 768, 46);
    lv_obj_set_style_bg_color(strip, lv_color_make(70, 12, 12), 0);
    lv_obj_set_style_bg_opa(strip, 245, 0);
    lv_obj_set_style_border_width(strip, 3, 0);
    lv_obj_set_style_radius(strip, 10, 0);
    lv_obj_set_style_shadow_color(strip, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(strip, 12, 0);
    lv_obj_set_style_shadow_opa(strip, LV_OPA_40, 0);
    lv_obj_set_style_shadow_ofs_y(strip, 3, 0);
    lv_obj_set_style_pad_hor(strip, 10, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *pill = lv_obj_create(strip);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 68, 30);
    lv_obj_set_style_bg_color(pill, lv_color_make(230, 50, 50), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(pill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *l = make_label(pill, &sony_24, 0xFFFFFF);
    lv_label_set_text(l, "TX");
    lv_obj_center(l);

    l = make_label(strip, &sony_18, 0x9A9A9A);
    lv_label_set_text(l, "PWR");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 82, 0);

    lv_obj_t *track = lv_obj_create(strip);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, STRIP_BAR_W, 14);
    lv_obj_set_style_bg_color(track, lv_color_make(30, 30, 30), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_align(track, LV_ALIGN_LEFT_MID, 128, 0);
    strip_fill = lv_obj_create(track);
    lv_obj_remove_style_all(strip_fill);
    lv_obj_set_size(strip_fill, 0, 14);
    lv_obj_set_style_bg_color(strip_fill, lv_color_make(80, 200, 110), 0);
    lv_obj_set_style_bg_opa(strip_fill, LV_OPA_COVER, 0);

    strip_pwr = make_label(strip, &sony_30, 0xFFFFFF);
    lv_obj_set_width(strip_pwr, 100);
    lv_obj_set_style_text_align(strip_pwr, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(strip_pwr, LV_ALIGN_LEFT_MID, 394, 0);

    l = make_label(strip, &sony_18, 0x9A9A9A);
    lv_label_set_text(l, "SWR");
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 520, 0);
    strip_swr = make_label(strip, &sony_30, 0xFFFFFF);
    lv_obj_align(strip_swr, LV_ALIGN_LEFT_MID, 566, 0);

    strip_time = make_label(strip, &sony_28, 0xFFFFFF);
    lv_obj_align(strip_time, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_event_cb(obj, radio_cb, EVENT_RADIO_TX, NULL);
    lv_obj_add_event_cb(obj, radio_cb, EVENT_RADIO_RX, NULL);
    subject_subscribe((Subject *)cfg_cur_mode, mode_changed, NULL);
    return obj;
}

/* From the base, in its thread. The ALC from 0 (at rest) up: the X6100 backend turns its count round, the
 * X6200 sends it this way (as gdyuldin's X6200 GUI reads it, 34ec351 "Invert ALC") */
void tx_info_update(float p, float s, float a) {
    s = LV_MIN(SWR_MAX, s);
    if (cur_mode == radio_mode_lsb_dig || cur_mode == radio_mode_usb_dig) {
        pwr = p;
        alc = a;
        vswr = s;
    } else {
        lpf(&pwr, p, 0.9f, 0.0f);
        lpf(&alc, a, 0.9f, 0.0f);
        lpf(&vswr, s, 0.9f, 0.0f);
    }
    msg_id++;
    scheduler_put_coalesced(update_tx_info);
}

void tx_info_get(float *p, float *s) {
    *p = pwr;
    *s = vswr;
}

bool tx_info_refresh(uint8_t *seen, float *alc_p, float *pwr_p, float *vswr_p) {
    if (*seen == msg_id) return false;
    *seen = msg_id;
    if (alc_p) *alc_p = alc;
    if (pwr_p) *pwr_p = pwr;
    if (vswr_p) *vswr_p = vswr;
    return true;
}

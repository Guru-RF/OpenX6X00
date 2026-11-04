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
#include "smeter.h"            /* meter_tx_redraw */
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

static lv_obj_t     *obj;        /* carries the TX and RX events, never shown */
static lv_obj_t     *panel;      /* big TX panel under the top bars */
static lv_obj_t     *head_label;
static lv_obj_t     *time_label;
static lv_obj_t     *pwr_value;
static lv_obj_t     *swr_value;
static lv_obj_t     *alc_value;
static lv_obj_t     *alc_name;
static uint32_t      tx_start;
static float         tx_len;         /* tx_info_set_length */
static float         tune_w, tune_len;   /* tx_info_set_tune */

/* Digital apps: a strip under the frequency bar instead of the panel */
static lv_obj_t     *strip;
static lv_obj_t     *strip_fill;
static lv_obj_t     *strip_pwr;
static lv_obj_t     *strip_swr;
static lv_obj_t     *strip_time;
static lv_obj_t     *strip_tag;      /* TX, or TUNE */
static lv_obj_t     *badge;          /* the panel's */

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

/* The three readings: names and scales */
static const char *const row_name[] = {"PWR", "SWR", "ALC"};
static const float       row_min[]  = {0.0f, SWR_MIN, 0.0f};
static const float       row_max[]  = {PWR_MAX, SWR_MAX, ALC_MAX};

static float row_val(int row) {
    return row == 0 ? pwr : row == 1 ? vswr : alc;
}

/* SWR in its own colour (web Display page), else the theme's; after dl2zw's SWR colour, OpenX6100 */
static uint32_t row_color(int row) {
    int32_t swr_own = param_i_get(cfg_swr_color);

    if (row == 1) return swr_own < 0 ? theme_hex(TC_line) : (uint32_t)swr_own;
    if (row == 2) return alc_greyed() ? theme_hex(TC_text_mute) : theme_hex(TC_alc);
    return theme_hex(TC_line);
}

/* One bar row, w x h in slices, lit up to the reading; SWR turns yellow then red. */
static void draw_bar(lv_draw_ctx_t *draw_ctx, lv_coord_t x1, lv_coord_t y1, lv_coord_t w, lv_coord_t h, int slices,
                     int row) {
    lv_draw_rect_dsc_t rect_dsc;
    lv_area_t          area;
    float              min = row_min[row], max = row_max[row];
    bool               swr = row == 1;
    uint32_t           on_color = row_color(row);
    lv_coord_t         slice_w = w / slices;
    int                lit = (LV_MAX(row_val(row), min) - min) / (max - min) * slices + 0.5f;

    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_COVER;
    area.y1 = y1;
    area.y2 = y1 + h - 1;

    for (int i = 0; i < slices; i++) {
        float v = min + (max - min) * i / slices;

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

    for (int i = 0; i < 3; i++) {
        draw_bar(draw_ctx, x1, y1 + i * ROW_STEP, BARS_W, 20, SLICES, i);
    }

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
    float     tune = tune_len;

    split_freq(f, &mhz, &khz, &hz);
    lv_label_set_text_fmt(head_label, "%s - %i.%03i.%03i", tune > 0.0f ? "EXTERNAL TUNE" : info_params_mode_label_get(),
                          mhz, khz, hz);
    if (tune > 0.0f)
        lv_label_set_text_fmt(time_label, "%u / %u s", (unsigned)s, (unsigned)(tune + 0.5f));
    else
        lv_label_set_text_fmt(time_label, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    lv_label_set_text(badge, tune > 0.0f ? "TUNE" : "TX");
}

static void update_strip(void) {
    uint32_t s   = lv_tick_elaps(tx_start) / 1000;
    float    tune = tune_len, len = tune > 0.0f ? tune : tx_len;
    float    ref = LV_MAX(tune > 0.0f ? tune_w : param_f_get(cfg_pwr), 0.1f);
    /* SWR from 2 yellow, from 3 red (as the panel's bar and the walkie) */
    lv_color_t c = vswr >= 3.0f ? TC(bar_high) : vswr >= 2.0f ? TC(bar_mid) : lv_color_white();

    lv_obj_set_width(strip_fill, STRIP_BAR_W * LV_MIN(pwr / ref, 1.0f));
    lv_label_set_text_fmt(strip_pwr, "%.1f W", pwr);
    lv_label_set_text_fmt(strip_swr, "%.1f", vswr);
    lv_obj_set_style_text_color(strip_swr, c, 0);
    lv_obj_set_style_border_color(strip, vswr >= 2.0f ? c : lv_color_make(230, 50, 50), 0);
    if (len > 0.0f)
        lv_label_set_text_fmt(strip_time, "%u / %u s", (unsigned)s, (unsigned)(len + 0.5f));
    else
        lv_label_set_text_fmt(strip_time, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    lv_label_set_text(strip_tag, tune > 0.0f ? "TUNE" : "TX");
}

void tx_info_set_length(float seconds) {
    tx_len = seconds;
}

/* An app's slot that starts during a tune is refused (tx_player), but has set its length by then: the tune's
 * comes first */
void tx_info_set_tune(float w, float seconds) {
    tune_w   = w;
    tune_len = seconds;
}

/* The digital apps keep their screen while transmitting: the strip, not the panel */
static bool digital_app_open(void) {
    static dialog_t **const apps[] = {&dialog_ft8, &dialog_js8, &dialog_wspr, &dialog_sstv,
                                      &dialog_psk, &dialog_olivia, &dialog_jtty};

    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++)
        if (dialog_type_is_run(*apps[i])) return true;
    return rtty_get_state() != RTTY_OFF;
}

/* This TX on the apps' strip, or on the panel with Settings > Display > INTERFACE > Popups TX on; else the S-meter
 * box shows the readings, once and not twice (#166). From the apps open and the setting, not from what is on screen:
 * the S-meter asks first (screen_main.c rx_tx_cb) */
bool tx_info_shown(void) {
    return digital_app_open() || params.mag_alc.x;
}

/* TX: the readings from zero, the strip or the panel (under the frequency strip), or neither: tx_info_shown().
 * RX: all hidden */
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
    if (!tx_info_shown()) return;   /* the S-meter box has them */
    /* Framed red for TX. VISION: under the S-meter row; deck: under the frequency panel, off its S/SWR/ALC scales */
    lv_obj_set_y(panel, theme_shape.deck ? FREQ_STRIP_Y + FREQ_STRIP_H + 8 : DIALOG_Y + 8 + METER_ROW_H);
    update_head();
    lv_obj_set_style_text_color(alc_name, alc_greyed() ? TC(text_mute) : TC(text_value), 0);
    lv_obj_set_style_text_color(alc_value, alc_greyed() ? TC(text_mute) : TC(text), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel);
}

/* In the LVGL thread: the new readings on the S-meter box and on whichever view is shown */
static void update_tx_info(void *unused) {
    meter_tx_redraw();
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

/* Text in a cell, centred up and down */
static void cell_text(lv_draw_ctx_t *draw_ctx, const lv_font_t *font, lv_color_t color, lv_text_align_t align,
                      lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, const char *text) {
    lv_draw_label_dsc_t dsc;
    lv_coord_t          line = lv_font_get_line_height(font);
    lv_area_t           a = {.x1 = x, .y1 = y + (h - line) / 2, .x2 = x + w - 1};

    a.y2 = a.y1 + line - 1;
    lv_draw_label_dsc_init(&dsc);
    dsc.font  = font;
    dsc.color = color;
    dsc.align = align;
    lv_draw_label(draw_ctx, &dsc, &a, text, NULL);
}

/* The S-meter box while transmitting (smeter.c; the deck's scales draw their own): the three readings, each with
 * its name and value, in rows with the panel's bars, or side by side in a wide, low box (VISION's meter row): no
 * room for bars there, the values big beside their names */
void tx_meters_draw(lv_obj_t *box, lv_draw_ctx_t *draw_ctx) {
    lv_coord_t       pad = lv_obj_get_style_border_width(box, LV_PART_MAIN) + 1;
    lv_coord_t       x0 = box->coords.x1 + pad + 5, y0 = box->coords.y1 + pad;
    lv_coord_t       w = lv_obj_get_width(box) - 2 * (pad + 5), h = lv_obj_get_height(box) - 2 * pad;
    bool             side = w > 6 * h;
    const lv_font_t *font = side ? &sony_16 : &sony_18, *value_font = side ? &sony_24 : font;
    lv_coord_t       gap = side ? 24 : 0;     /* between the cells side by side */
    lv_coord_t       cell_w = side ? (w - 2 * gap) / 3 : w, cell_h = side ? h : h / 3;
    lv_point_t       sz;
    char             text[12];

    lv_txt_get_size(&sz, "PWR", font, 0, 0, LV_COORD_MAX, 0);
    lv_coord_t name_w = sz.x + 8;
    lv_txt_get_size(&sz, "10.0 W", value_font, 0, 0, LV_COORD_MAX, 0);
    lv_coord_t value_w = sz.x;
    lv_coord_t bar_w = cell_w - name_w - value_w - 6;
    lv_coord_t bar_h = LV_MIN(10, cell_h - 6);

    for (int i = 0; i < 3; i++) {
        lv_coord_t x = side ? x0 + i * (cell_w + gap) : x0, y = side ? y0 : y0 + i * cell_h;
        float      v = row_val(i);
        bool       grey = i == 2 && alc_greyed();

        cell_text(draw_ctx, font, grey ? TC(text_mute) : TC(text_value), LV_TEXT_ALIGN_LEFT, x, y, name_w, cell_h,
                  row_name[i]);
        if (!side) {
            draw_bar(draw_ctx, x + name_w, y + (cell_h - bar_h) / 2, bar_w, bar_h, LV_CLAMP(10, bar_w / 6, SLICES), i);
        }
        /* SWR from 2 yellow, from 3 red, as its bar */
        snprintf(text, sizeof(text), i ? "%.1f" : "%.1f W", v);
        cell_text(draw_ctx, value_font,
                  i == 1 && v >= 3.0f ? TC(bar_high) : i == 1 && v >= 2.0f ? TC(bar_mid) : grey ? TC(text_mute) : TC(text),
                  side ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, side ? x + name_w : x + cell_w - value_w, y, value_w,
                  cell_h, text);
    }
}

lv_obj_t *tx_info_init(lv_obj_t *parent) {
    /* Carries the TX/RX events (the S-meter box shows the readings, framed red: smeter.c) */
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

    badge = make_label(panel, &sony_22, theme_hex(TC_text));
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
    lv_obj_set_size(pill, 72, 30);
    lv_obj_set_style_bg_color(pill, lv_color_make(230, 50, 50), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(pill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *l = strip_tag = make_label(pill, &sony_24, 0xFFFFFF);
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

float tx_info_get_alc(bool *greyed) {
    *greyed = alc_greyed();
    return alc;
}

bool tx_info_refresh(uint8_t *seen, float *alc_p, float *pwr_p, float *vswr_p) {
    if (*seen == msg_id) return false;
    *seen = msg_id;
    if (alc_p) *alc_p = alc;
    if (pwr_p) *pwr_p = pwr;
    if (vswr_p) *vswr_p = vswr;
    return true;
}

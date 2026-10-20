/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - SWR scan: the antenna SWR over a span around the VFO
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "swrscan_window.h"

#include "gui_styles.h"
#include "theme.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "gui_events.h"
#include "key_input.h"
#include "screen_main.h"
#include "button_bar.h"

#include <math.h>
#include <stdio.h>

#define POINTS      50
#define TOP_H       34      /* the frequencies above the grid */
#define BOTTOM_H    10

static lv_obj_t *plot;
static float    swr[POINTS];        /* as measured */
static float    smooth[POINTS];     /* five points averaged, two behind the scan */
static uint16_t pos;
static uint32_t f_center, f_low, f_high;
static bool     scanning;
static bool     lin_scale;          /* else log */
static int32_t  span_hz;

static ObserverDelayed *freq_obs, *linear_obs, *span_obs;

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void plot_key_cb(lv_event_t *e);
static void run_cb(button_data_t *b);
static void scale_cb(button_data_t *b);
static void span_cb(button_data_t *b);
static const char *scale_label();
static const char *span_label();

static button_data_t btn_run   = { .type = BTN_TEXT,    .label = "Run",             .press = run_cb };
static button_data_t btn_scale = { .type = BTN_TEXT_FN, .label_fn = scale_label,    .press = scale_cb };
static button_data_t btn_span  = { .type = BTN_TEXT_FN, .label_fn = span_label,     .press = span_cb };

static buttons_page_t page = { { &btn_run, &btn_scale, &btn_span } };

static dialog_t dialog = { .construct_cb = open_cb, .destruct_cb = close_cb, .btn_page = &page, .key_cb = plot_key_cb };

dialog_t *dialog_swrscan = &dialog;

/* A flat 1.0 line and the scan back at the low edge of the span */
static void reset_scan() {
    for (int i = 0; i < POINTS; i++) swr[i] = smooth[i] = 1.0f;
    pos = 0;
    f_center = cparam_i_get(cfg_fg_freq);
    f_low = f_center - span_hz / 2;
    f_high = f_center + span_hz / 2;
}

/* SWR to the plot's y from its top: 1 at the bottom, 5 at the top in linear */
static lv_coord_t swr_y(float v, lv_coord_t h) {
    float k;

    if (lin_scale)
        k = (v - 1.0f) / 4.0f;
    else
        k = log10f(1.0f + (v - 1.0f) / (1.0f / logf(10.0)));
    return (1.0f - k) * h;
}

static void draw_text(lv_draw_ctx_t *ctx, lv_draw_label_dsc_t *dsc, lv_coord_t x, lv_coord_t y_mid,
                      lv_coord_t y_top, const char *s, bool centred)
{
    lv_point_t  size;
    lv_area_t   a;

    lv_txt_get_size(&size, s, dsc->font, 0, 0, LV_COORD_MAX, 0);
    if (centred) {
        a.x1 = x - size.x / 2;
        a.y1 = y_top;
        a.x2 = a.x1 + size.x;
        a.y2 = a.y1 + size.y;
    } else {
        a.x1 = x;
        a.y1 = y_mid - size.y / 2;
        a.x2 = x + size.x;
        a.y2 = y_mid + size.y / 2;
    }
    lv_draw_label(ctx, dsc, &a, s, NULL);
}

/* Drawn over the plot's empty box: zones, grid, the three frequencies, the curve */
static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t       *ctx = lv_event_get_draw_ctx(e);
    lv_coord_t          x0 = plot->coords.x1;
    lv_coord_t          top = plot->coords.y1;
    lv_coord_t          y0 = top + TOP_H;
    lv_coord_t          w = lv_area_get_width(&plot->coords);
    lv_coord_t          h = lv_area_get_height(&plot->coords) - TOP_H - BOTTOM_H;
    lv_draw_rect_dsc_t  zone;
    lv_draw_line_dsc_t  line;
    lv_draw_label_dsc_t text;
    lv_area_t           a;
    lv_point_t          p1, p2;
    char                s[32];

    /* Zones in the theme colours: up to 1.5 good, over 3 bad */
    lv_draw_rect_dsc_init(&zone);
    zone.bg_opa = LV_OPA_20;
    a.x1 = x0;
    a.x2 = x0 + w;
    a.y1 = y0 + swr_y(1.5f, h);
    a.y2 = y0 + h;
    zone.bg_color = TC(ok);
    lv_draw_rect(ctx, &zone, &a);
    a.y1 = y0;
    a.y2 = y0 + swr_y(3.0f, h);
    zone.bg_color = TC(err);
    lv_draw_rect(ctx, &zone, &a);

    /* SWR lines every 0.5, each after its number */
    lv_draw_label_dsc_init(&text);
    text.color = TC(text_dim);
    text.font = &sony_20;
    lv_draw_line_dsc_init(&line);
    line.color = TC(line);
    line.width = 1;
    line.opa = LV_OPA_60;

    for (float v = 1.5f; v <= 4.5f; v += 0.5f) {
        lv_point_t size;

        snprintf(s, sizeof(s), "%.1f", v);
        lv_txt_get_size(&size, s, text.font, 0, 0, LV_COORD_MAX, 0);
        p1.x = x0 + size.x + 6;
        p1.y = p2.y = y0 + swr_y(v, h);
        p2.x = x0 + w;
        lv_draw_line(ctx, &line, &p1, &p2);
        draw_text(ctx, &text, x0, p1.y, 0, s, false);
    }

    /* Three frequencies at the quarters, the VFO one in the middle stronger */
    uint32_t f = f_center - span_hz / 4;

    for (int q = -1; q <= 1; q++, f += span_hz / 4) {
        p1.x = p2.x = x0 + w / 2 + (w / 4) * q;
        p1.y = top + 4 + 24;
        p2.y = y0 + h;
        line.color = q ? TC(line) : TC(vfo_line);
        line.opa = q ? LV_OPA_60 : LV_OPA_COVER;
        lv_draw_line(ctx, &line, &p1, &p2);

        text.color = q ? TC(text_dim) : TC(text);
        snprintf(s, sizeof(s), "%i.%03i.%03i", (int)(f / 1000000), (int)(f / 1000 % 1000), (int)(f % 1000));
        draw_text(ctx, &text, p1.x, 0, top + 4, s, true);
    }

    /* The curve, each piece in the colour of its worse end */
    line.width = 4;
    line.opa = LV_OPA_COVER;
    line.round_start = 1;
    line.round_end = 1;

    for (int i = 1; i < POINTS; i++) {
        float v = LV_MAX(smooth[i - 1], smooth[i]);

        line.color = v <= 1.5f ? TC(ok) : v <= 3.0f ? TC(warn) : TC(err);
        p1.x = x0 + (i - 1) * w / POINTS;
        p1.y = y0 + swr_y(smooth[i - 1], h);
        p2.x = x0 + i * w / POINTS;
        p2.y = y0 + swr_y(smooth[i], h);
        lv_draw_line(ctx, &line, &p1, &p2);
    }
}

/* One reading at the current point, then on to the next frequency */
static void scan_step(float v) {
    int mid = (POINTS + pos - 2) % POINTS;
    float sum = 0.0f;

    swr[pos] = v;
    for (int i = mid - 2; i <= mid + 2; i++)
        sum += swr[LV_CLAMP(0, i, POINTS - 1)];
    smooth[mid] = sum / 5.0f;
    event_send(plot, LV_EVENT_REFRESH, NULL);

    pos = (pos + 1) % POINTS;
    radio_set_freq(f_low + (f_high - f_low) * pos / POINTS);
}

static void freq_changed(Subject *subj, void *user_data) {
    reset_scan();
    lv_obj_invalidate(plot);
}

static void linear_changed(Subject *subj, void *user_data) {
    lin_scale = subject_i_get((SubjectInt *)subj);
}

static void span_changed(Subject *subj, void *user_data) {
    span_hz = subject_i_get((SubjectInt *)subj);
}

/* An empty box the size of the dialog: draw_cb paints it, it takes the keys */
static lv_obj_t *make_plot(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_add_event_cb(box, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_set_size(box, 780, DIALOG_H - 8);    /* the frequencies on top stay inside the dialog */
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_group_add_obj(keyboard_group, box);
    lv_obj_add_event_cb(box, plot_key_cb, LV_EVENT_KEY, NULL);
    return box;
}

static void open_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    /* the Scale and Span labels follow their settings, the plot reads them too */
    btn_scale.subj = (Subject *)cfg_swrscan_linear;
    btn_span.subj = (Subject *)cfg_swrscan_span;
    linear_obs = subject_subscribe_delayed_and_notify((Subject *)cfg_swrscan_linear, linear_changed, NULL);
    span_obs = subject_subscribe_delayed_and_notify((Subject *)cfg_swrscan_span, span_changed, NULL);
    buttons_load_page(&page);

    freq_obs = subject_subscribe_delayed((Subject *)cfg_fg_freq, freq_changed, NULL);
    plot = make_plot(dialog.obj);
    reset_scan();
}

static void unsubscribe(ObserverDelayed **obs) {
    if (*obs) param_unsubscribe((Observer *)*obs);
    *obs = NULL;
}

/* A scan still running stops first; the VFO goes back to its frequency */
static void close_cb(void) {
    if (scanning) run_cb(NULL);
    unsubscribe(&freq_obs);
    unsubscribe(&linear_obs);
    unsubscribe(&span_obs);
    radio_set_freq(cparam_i_get(cfg_fg_freq));
}

static void plot_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}

/* Run / stop. The VFO settings are kept aside while scanning and put back after */
static void run_cb(button_data_t *b) {
    if (scanning) {
        scanning = false;
        radio_stop_swrscan();
        radio_set_freq(f_center);
        mem_load(MEM_BACKUP_ID);
    } else {
        mem_save(MEM_BACKUP_ID);
        reset_scan();
        radio_set_freq(f_low);
        scanning = radio_start_swrscan();
    }
}

static void scale_cb(button_data_t *b) {
    param_i_set(cfg_swrscan_linear, !param_i_get(cfg_swrscan_linear));
}

static void span_cb(button_data_t *b) {
    static const int32_t spans[] = { 50000, 100000, 200000, 500000 };
    int32_t cur;

    if (scanning) return;
    cur = param_i_get(cfg_swrscan_span);
    for (int i = 0; i < 4; i++) {
        if (spans[i] == cur) {
            cur = spans[(i + 1) % 4];
            break;
        }
    }
    param_i_set(cfg_swrscan_span, cur);

    reset_scan();   /* the span is still the old one here: the observer is delayed */
    event_send(plot, LV_EVENT_REFRESH, NULL);
}

static const char *scale_label() {
    return param_i_get(cfg_swrscan_linear) ? "Scale:\nLinear" : "Scale:\nLog";
}

static const char *span_label() {
    static char s[20];

    snprintf(s, sizeof(s), "Span:\n%u kHz", param_i_get(cfg_swrscan_span) / 1000);
    return s;
}

void dialog_swrscan_update(float vswr) {
    if (scanning) scan_step(vswr);
}

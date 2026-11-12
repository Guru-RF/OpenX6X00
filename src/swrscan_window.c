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
#include "msg.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define POINTS      50      /* across a span under 1 MHz */
#ifdef BOARD_X6200
#define POINTS_SLOW 25      /* the same with the X6200's carrier: each point waits for the base's power, ~1 s */
#endif
#define POINTS_WIDE 100     /* from 1 MHz: one every 25 kHz at 2.5 MHz, so a narrow dip still shows (#107) */
#define TOP_H       34      /* the frequencies above the grid */
#define BOTTOM_H    10

static lv_obj_t *plot;
static float    swr[POINTS_WIDE];       /* as measured */
static float    smooth[POINTS_WIDE];    /* the reading, averaged with its neighbours when there are many */
static bool     measured[POINTS_WIDE];  /* a reading came for it: only these are drawn */
static uint16_t points = POINTS;        /* in this sweep */
static uint16_t order[POINTS_WIDE];     /* the points in the order they are read */
static uint16_t order_at;
static uint16_t pos;
static int32_t  f_center, f_low, f_high;
static bool     scanning;
static atomic_bool moved;                /* the dial moved the sweep: the reading on its way is the old frequency's */
static bool     lin_scale;          /* else log */
static int32_t  span_hz;

static ObserverDelayed *freq_obs, *linear_obs, *span_obs;

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void plot_key_cb(lv_event_t *e);
static void run_cb(button_data_t *b);
static void scale_cb(button_data_t *b);
static void span_cb(button_data_t *b);
static const char *run_label();
static const char *scale_label();
static const char *span_label();
#ifdef BOARD_X6200
static void sweep_cb(button_data_t *b);
static const char *sweep_label();
#endif

static button_data_t btn_run   = { .type = BTN_TEXT_FN, .label_fn = run_label,      .press = run_cb };
static button_data_t btn_scale = { .type = BTN_TEXT_FN, .label_fn = scale_label,    .press = scale_cb };
static button_data_t btn_span  = { .type = BTN_TEXT_FN, .label_fn = span_label,     .press = span_cb };

#ifdef BOARD_X6200
/* Carrier, a plain FM carrier read once the power is up, or Quick, the base's own sweep (radio_start_swrscan, #212) */
static button_data_t btn_sweep = { .type = BTN_TEXT_FN, .label_fn = sweep_label,    .press = sweep_cb };

static buttons_page_t page = { { &btn_run, &btn_scale, &btn_span, &btn_sweep } };
#else
static buttons_page_t page = { { &btn_run, &btn_scale, &btn_span } };
#endif

static dialog_t dialog = { .construct_cb = open_cb, .destruct_cb = close_cb, .btn_page = &page, .key_cb = plot_key_cb };

dialog_t *dialog_swrscan = &dialog;

/* Whether the sweep keys the X6200's carrier, a point in ~1 s (Sweep: Carrier), not the base's own sweep */
static bool slow_points() {
#ifdef BOARD_X6200
    return !param_i_get(cfg_swrscan_quick);
#else
    return false;
#endif
}

/* With the X6200's carrier, where a point takes ~1 s: the two ends, then every 2^n-th point, then the points halfway
 * between those, so that the shape is there after a few seconds. Elsewhere from the low edge up, the base's own scan */
static void make_order() {
    int n = 0;

    if (slow_points()) {
        int top = 1;

        while (top * 2 < points - 1) top *= 2;
        order[n++] = 0;
        order[n++] = points - 1;
        for (int step = top; step >= 1; step /= 2) {
            for (int i = step; i < points - 1; i += step) {
                if (step == top || i % (step * 2) == step) order[n++] = i;
            }
        }
    } else {
        for (int i = 0; i < points; i++) order[n++] = i;
    }
    order_at = 0;
    pos = order[0];
}

/* No curve yet and the scan back at the low edge of the span, around the frequency the radio transmits on
 * (the other VFO in split, a TX shift's). The sweep stays where the radio transmits the same way (#107): a span past
 * the edge of HF (0.1 or 55 MHz), of a transverter's range or of the amateur band (with TX outside the amateur bands
 * off, #121) moves inside it, and one wider than the range takes all of it; the frequencies on the plot say so. On an
 * amateur band that is the band list's band, which follows the IARU region (cfg_tx_scan_range): 2.5 MHz on 40 m
 * sweeps 7.0-7.2 in Region 1. Up to v0.3.9 it swept the table's 7.0-7.3, into the 41 m broadcast band. A point
 * outside used to be refused by the radio, and the scan read the last frequency again. The points as many as the
 * sweep's own width wants */
static void reset_scan() {
    int32_t lo, hi;

    f_center = radio_tx_freq();
    f_low = f_center - span_hz / 2;
    f_high = f_center + span_hz / 2;
    if (cfg_tx_scan_range(f_center, &lo, &hi)) {
        if (f_low < lo) {
            f_high = LV_MIN(f_high + (lo - f_low), hi);
            f_low = lo;
        } else if (f_high > hi) {
            f_low = LV_MAX(f_low - (f_high - hi), lo);
            f_high = hi;
        }
    }
#ifdef BOARD_X6200
    points = f_high - f_low >= 1000000 ? POINTS_WIDE : slow_points() ? POINTS_SLOW : POINTS;
#else
    points = f_high - f_low >= 1000000 ? POINTS_WIDE : POINTS;
#endif
    memset(measured, 0, sizeof(measured));
    make_order();
}

/* The plot's x of a frequency in the sweep */
static lv_coord_t freq_x(int32_t f, lv_coord_t x0, lv_coord_t w) {
    return x0 + (int64_t)(f - f_low) * w / (f_high - f_low);
}

/* SWR to the plot's y from its top: 1 at the bottom, 5 at the top in linear */
static lv_coord_t swr_y(float v, lv_coord_t h) {
    float k;

    if (lin_scale)
        k = (v - 1.0f) / 4.0f;
    else
        k = log10f(1.0f + (v - 1.0f) / (1.0f / logf(10.0)));
    return (1.0f - LV_CLAMP(0.0f, k, 1.0f)) * h;    /* over 5 stays on the plot, off the frequencies above it */
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
    if (theme_shape.mono) zone.bg_opa = LV_OPA_30;   /* 1bit: the bad zone stippled (the good one goes) */
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

    /* Three frequencies at the quarters of the sweep, the VFO's in the middle stronger. A sweep moved inside the TX
     * range (reset_scan) has the VFO elsewhere: its line is drawn there, without a number */
    bool vfo_drawn = false;

    for (int q = 1; q <= 3; q++) {
        int32_t f = f_low + (f_high - f_low) / 4 * q;
        bool    vfo = f == f_center;

        vfo_drawn |= vfo;
        p1.x = p2.x = x0 + w * q / 4;
        p1.y = top + 4 + 24;
        p2.y = y0 + h;
        line.color = vfo ? TC(vfo_line) : TC(line);
        line.opa = vfo ? LV_OPA_COVER : LV_OPA_60;
        lv_draw_line(ctx, &line, &p1, &p2);

        text.color = vfo ? TC(text) : TC(text_dim);
        snprintf(s, sizeof(s), "%i.%03i.%03i", (int)(f / 1000000), (int)(f / 1000 % 1000), (int)(f % 1000));
        draw_text(ctx, &text, p1.x, 0, top + 4, s, true);
    }
    if (!vfo_drawn && f_center >= f_low && f_center <= f_high) {
        p1.x = p2.x = freq_x(f_center, x0, w);
        line.color = TC(vfo_line);
        line.opa = LV_OPA_COVER;
        lv_draw_line(ctx, &line, &p1, &p2);
    }

    /* The point being read, as the stock firmware's scan indicator: the order jumps about on the X6200 (make_order)
     * and the curve is redrawn over itself, so without it a sweep at work and one that has stopped looked the same */
    if (scanning) {
        int at = LV_MIN(pos, points - 1);   /* the radio thread moves it */

        p1.x = p2.x = x0 + at * w / (points - 1);
        p1.y = y0;
        p2.y = y0 + h;
        line.color = TC(text);
        line.opa = LV_OPA_50;
        line.width = 2;
        lv_draw_line(ctx, &line, &p1, &p2);
        line.width = 1;
    }

    /* The curve, each piece in the colour of its worse end */
    line.width = 4;
    line.opa = LV_OPA_COVER;
    line.round_start = 1;
    line.round_end = 1;

    /* Through what was measured, however few: a rounded line (a cubic per gap, its slopes the harmonic mean of the
     * two sides, 0 at a peak or dip, so that it never swings past the readings and invents a dip) */
    int   idx[POINTS_WIDE], n = 0;
    float d[POINTS_WIDE];

    for (int i = 0; i < points; i++) {
        if (measured[i]) idx[n++] = i;
    }
    for (int k = 1; k < n - 1; k++) {
        float s0 = (smooth[idx[k]] - smooth[idx[k - 1]]) / (idx[k] - idx[k - 1]);
        float s1 = (smooth[idx[k + 1]] - smooth[idx[k]]) / (idx[k + 1] - idx[k]);

        d[k] = s0 * s1 <= 0.0f ? 0.0f : 2.0f * s0 * s1 / (s0 + s1);
    }
    if (n > 1) {
        d[0] = (smooth[idx[1]] - smooth[idx[0]]) / (idx[1] - idx[0]);
        d[n - 1] = (smooth[idx[n - 1]] - smooth[idx[n - 2]]) / (idx[n - 1] - idx[n - 2]);
    }
    for (int k = 1; k < n; k++) {
        int   from = idx[k - 1], gap = idx[k] - from;
        int   steps = LV_MAX(1, gap * w / (points - 1) / 8);
        float v0 = smooth[from], v1 = smooth[idx[k]], prev = v0;

        p1.x = x0 + from * w / (points - 1);
        p1.y = y0 + swr_y(prev, h);
        for (int part = 1; part <= steps; part++) {
            float t = (float)part / steps, t2 = t * t, t3 = t2 * t;
            float v = (2 * t3 - 3 * t2 + 1) * v0 + (t3 - 2 * t2 + t) * gap * d[k - 1] +
                      (-2 * t3 + 3 * t2) * v1 + (t3 - t2) * gap * d[k];

            v = LV_MAX(v, 1.0f);
            line.color = LV_MAX(prev, v) <= 1.5f ? TC(ok) : LV_MAX(prev, v) <= 3.0f ? TC(warn) : TC(err);
            p2.x = x0 + (lv_coord_t)((from + gap * t) * w / (points - 1));
            p2.y = y0 + swr_y(v, h);
            lv_draw_line(ctx, &line, &p1, &p2);
            p1 = p2;
            prev = v;
        }
    }
}

/* The sweep's points, from f_low to f_high itself: the last one used to stop a point short of the top */
static int32_t point_freq(int i) {
    return f_low + (int64_t)(f_high - f_low) * i / (points - 1);
}

/* One reading at the current point, then on to the next frequency */
static void scan_step(float v) {
    const int reach = points <= 25 ? 0 : 2;     /* few points: an average lifted a dip of 1.5 to 1.7 */

    if (atomic_exchange(&moved, false)) {       /* read before the dial moved: the sweep starts over from here */
        radio_set_tx_freq(point_freq(pos));
        return;
    }
    v = LV_MAX(v, 1.0f);
    swr[pos] = smooth[pos] = v;     /* before measured: the plot draws from another thread */
    measured[pos] = true;
    for (int i = LV_MAX(pos - reach, 0); i <= LV_MIN(pos + reach, points - 1); i++) {
        float sum = 0.0f;
        int   n = 0;

        if (!measured[i]) continue;

        for (int j = LV_MAX(i - reach, 0); j <= LV_MIN(i + reach, points - 1); j++) {
            if (measured[j]) {
                sum += swr[j];
                n++;
            }
        }
        smooth[i] = sum / n;
    }
    event_send(plot, LV_EVENT_REFRESH, NULL);

    order_at = (order_at + 1) % points;
    /* the dial may have changed the sweep's points under this thread: never a frequency past its end */
    pos = LV_MIN(order[order_at], points - 1);
    radio_set_tx_freq(point_freq(pos));
}

/* The dial moved: the sweep around it again. Moved where the radio may not transmit (#121), the scan stops, as if
 * Stop was pressed: the VFO settings back as before the scan, the dial with them */
static void freq_changed(Subject *subj, void *user_data) {
    int32_t tx = radio_tx_freq();

    if (scanning && cfg_tx_refusal(tx)) {
        dialog_swrscan_stop(tx);
        return;
    }
    reset_scan();
    if (scanning) atomic_store(&moved, true);
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

    /* the Scale and Span labels follow their settings (Sweep on the X6200 too), the plot reads them too */
    btn_scale.subj = (Subject *)cfg_swrscan_linear;
    btn_span.subj = (Subject *)cfg_swrscan_span;
#ifdef BOARD_X6200
    btn_sweep.subj = (Subject *)cfg_swrscan_quick;
#endif
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
    radio_set_tx_freq(0);
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

/* Run / stop. The VFO settings are kept aside while scanning and put back after; the sweep moves the VFO that
 * transmits, which goes back to its own frequency. A start that cannot be (a tune or TX on, a receive-only
 * frequency) says why and moves nothing: it used to keep "Run" and move the VFO to the low edge in silence (#25) */
static void run_cb(button_data_t *b) {
    if (scanning) {
        scanning = false;
        radio_stop_swrscan();
        radio_set_tx_freq(0);
        mem_load(MEM_BACKUP_ID);
        lv_obj_invalidate(plot);    /* the line on the point being read goes: it stayed on until the next redraw */
    } else if (radio_get_state() != RADIO_RX) {
        msg_update_text_fmt("SWR scan: not while transmitting or tuning");
    } else {
        mem_save(MEM_BACKUP_ID);
        reset_scan();
        atomic_store(&moved, false);
        scanning = radio_start_swrscan(f_low);
    }
    buttons_refresh(&btn_run);
}

static void scale_cb(button_data_t *b) {
    param_i_set(cfg_swrscan_linear, !param_i_get(cfg_swrscan_linear));
}

/* The next span, up to 2500 kHz as the stock firmware has them (#107); one not in the list goes to the first */
static void span_cb(button_data_t *b) {
    static const int32_t spans[] = { 50000, 100000, 200000, 500000, 1000000, 1500000, 2000000, 2500000 };
    const int            n = sizeof(spans) / sizeof(spans[0]);
    int32_t              cur, next = spans[0];

    if (scanning) return;
    cur = param_i_get(cfg_swrscan_span);
    for (int i = 0; i < n; i++) {
        if (spans[i] == cur) {
            next = spans[(i + 1) % n];
            break;
        }
    }
    param_i_set(cfg_swrscan_span, next);

    span_hz = next;     /* the observer is delayed: the sweep and its frequencies follow now */
    reset_scan();
    event_send(plot, LV_EVENT_REFRESH, NULL);
}

#ifdef BOARD_X6200
/* Not while scanning, as Span: the sweep under way keeps its way. Its points and their order follow at once */
static void sweep_cb(button_data_t *b) {
    if (scanning) return;
    param_i_set(cfg_swrscan_quick, !param_i_get(cfg_swrscan_quick));
    reset_scan();
    event_send(plot, LV_EVENT_REFRESH, NULL);
}

static const char *sweep_label() {
    return param_i_get(cfg_swrscan_quick) ? "Sweep:\nQuick" : "Sweep:\nCarrier";
}
#endif

static const char *run_label() {
    return scanning ? "Stop" : "Run";
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

void dialog_swrscan_stop(int32_t freq) {
    if (!scanning) return;
    run_cb(NULL);
    if (!radio_say_no_tx("Scan stopped", freq, true)) msg_update_text_fmt("Scan stopped");   /* fits whole (#121) */
}

void dialog_swrscan_give_up(const char *why) {
    if (!scanning) return;
    run_cb(NULL);
    msg_update_text_fmt("%s", why);
}

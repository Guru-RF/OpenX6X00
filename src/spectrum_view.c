/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 *
 *  The spectrum over the waterfall: the DSP's trace with its peaks, and on it the
 *  passband, the notch, the decoder tones and the VFO line. With the DRM planes the
 *  trace goes straight to the primary plane and LVGL draws only the markers.
 */

#include "spectrum_view.h"

#include "cfg/cfg_api.h"
#include "navtex_window.h"
#include "wefax_window.h"
#include "dsp.h"
#include "smeter.h"
#include "params/params.h"
#include "radio.h"
#include "recorder.h"
#include "rtty.h"
#include "scheduler.h"
#include "gui_styles.h"
#include "theme.h"
#include "util.h"
#include "waterfall_view.h"

#include "lv_drivers/display/drm.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define POINTS      800         /* points of the trace, one a pixel across the screen */
#define SPAN_HZ     100000      /* shown at zoom 1 */
#define TX_MIN      S4          /* the scale in TX, and the automatic one before the DSP sets it */
#define TX_MAX      S9_20
#define KEPT_SETTLE 8           /* traces after a retune not kept (kept_settle()) */

static lv_obj_t        *obj;

/* Written by the DSP thread */
static pthread_mutex_t  data_mux;
static float            trace_db[POINTS];
static float            peak_db[POINTS];
static uint64_t         peak_at[POINTS];    /* when each peak was last raised */
static bool             data_tx;
static uint8_t          last_fft_dec = 1;
static int32_t          base_lo_hz;         /* where the base is tuned, as the data say */
static int16_t          peak_rest_hz;       /* a move of the peaks smaller than a point, kept for the next */

static float            level_min = TX_MIN;
static float            level_max = TX_MAX;

/* Fixed mode keeps the view still while the VFO moves across it, and the base sends only the
 * span around the VFO: the side tuned away from gets no new trace (#10). There the trace last
 * received at each point of the view, settled after a retune (kept_settle()), dimmed, instead of
 * nothing. GUI thread; NAN: nothing */
static float            kept_db[POINTS];
static int32_t          kept_view;          /* the view's centre (Hz) they are points of; 0: none */
static uint8_t          kept_zoom;
static bool             kept_new;           /* a settled trace not kept yet (data_mux) */
static uint8_t          kept_wait;          /* traces to come before one is settled (data_mux) */

static uint8_t          zoom = 1;
static int32_t          pass_lo;            /* filter edges, Hz from the VFO, LSB below it */
static int32_t          pass_hi = 3000;
static int32_t          vfo_hz;
static int32_t          rit_hz;
static int32_t          lo_offset;
static int32_t          if_shift;
static int32_t          notch_on;
static int32_t          notch_hz;           /* with the sideband's sign */
static int32_t          notch_half;
static bool             vfo_line_on = true;

/* Direct mode (DRM planes, spectrum shown): the trace is drawn on the primary plane
 * under the GUI, like the waterfall, and LVGL draws only the markers over it, again
 * when they change. strip is the portrait image of the spectrum: landscape x is its
 * row 799 - x, landscape y (from the top) its column y */
static bool             direct;
static uint32_t        *strip;
static int32_t          strip_h;
static int32_t          drawn_markers[12];

/* Pixels from the centre of a view w wide for hz, rounded either way from 0 (half up, a VFO
 * tuned below the centre of the Fixed view had its trace a pixel to the right) */
static lv_coord_t hz_px(int32_t hz, lv_coord_t w) {
    int32_t v = hz * zoom * w;

    return (v + (v < 0 ? -SPAN_HZ / 2 : SPAN_HZ / 2)) / SPAN_HZ;
}

/* Where the VFO is: the markers follow it (and the fixed waterfall view) */
static lv_coord_t markers_px(lv_coord_t w) {
    return hz_px(lo_offset + if_shift + waterfall_view_shift(), w);
}

/* Where the data are: the base takes a few frames to follow a new frequency, and
 * until then they stay centred where it still is. 16 bits: a band jump wraps the
 * lag, for those few frames only */
static lv_coord_t trace_px(lv_coord_t w) {
    int16_t lag = 0;

    if (data_tx) {
        return markers_px(w);
    }
    if (base_lo_hz) {
        lag = base_lo_hz - (vfo_hz + lo_offset - if_shift + rit_hz);
    }
    return hz_px(lo_offset + lag + waterfall_view_shift(), w);
}

static void levels(float *min, float *max) {
    *min = data_tx ? TX_MIN : level_min;
    *max = data_tx ? TX_MAX : level_max;
}

/* Fixed mode: a new trace into kept_db at the points it is drawn on; another view, another
 * zoom or Centered mode: none kept. Before each refresh */
static void keep_trace(void) {
    int32_t view = param_i_get(cfg_wf_mode) ? vfo_hz - if_shift - waterfall_view_shift() : 0;

    if (view != kept_view || zoom != kept_zoom) {
        kept_view = view;
        kept_zoom = zoom;
        for (int i = 0; i < POINTS; i++) {
            kept_db[i] = NAN;
        }
    }
    if (!kept_view) {
        return;
    }
    lv_coord_t shift = trace_px(POINTS);

    pthread_mutex_lock(&data_mux);
    if (kept_new && !data_tx) {
        for (int x = LV_MAX(shift, 0); x < LV_MIN(POINTS + shift, POINTS); x++) {
            kept_db[x] = trace_db[x - shift];
        }
    }
    kept_new = false;
    pthread_mutex_unlock(&data_mux);
}

/* A retune: the next KEPT_SETTLE traces are not kept. The OEM X6100 base sends no frequency with
 * its data (nor does the X6200's spectrum), so dsp.cpp cannot move its smoothed trace (in dB, up
 * 0.6 and down 0.2 of the way a frame) with the retune: for a while the carriers left behind still
 * show a step away and those of the new place too low, and the base's first frame is of the old
 * place. Kept, the side tuned away from would hold that. 8 traces (about half a second) take a
 * carrier left behind down to a sixth of its height over the noise (0.8^8) */
static void kept_settle(void) {
    pthread_mutex_lock(&data_mux);
    kept_wait = KEPT_SETTLE;
    kept_new = false;
    pthread_mutex_unlock(&data_mux);
}

/* The points of the side tuned away from (to the one where the trace starts, so they join),
 * false when none; not in TX */
static bool kept_side(int *from, int *to) {
    lv_coord_t shift = trace_px(POINTS);

    if (!kept_view || data_tx || shift == 0) {
        return false;
    }
    *from = shift > 0 ? 0 : LV_MAX(POINTS + shift - 1, 0);
    *to = shift > 0 ? LV_MIN(shift + 1, POINTS) : POINTS;
    return true;
}

/* The trace's colour, half way to the background */
static lv_color_t kept_color(lv_color_t trace, lv_color_t bg) {
    return lv_color_mix(trace, bg, LV_OPA_50);
}

/* Settings (web Display page): the trace and the fill in their own colours, else the theme's */
static lv_color_t trace_color(void) {
    int32_t c = param_i_get(cfg_spectrum_line_color);

    return c < 0 || theme_shape.mono ? TC(line) : lv_color_hex(c);   /* 1bit: ink */
}

/* The fill in its own colour: the trace is drawn over it */
static bool fill_own(void) {
    return params.spectrum_filled.x && param_i_get(cfg_spectrum_fill_color) >= 0 && !theme_shape.mono;
}

static lv_color_t fill_color(void) {
    return fill_own() ? lv_color_hex(param_i_get(cfg_spectrum_fill_color)) : trace_color();
}

/* The fill fading from its colour at the trace to the background at the bottom, the trace drawn
 * over it (after R1CBU's gradient fill, OpenX6100) */
static bool fill_grad(void) {
    return params.spectrum_filled.x && (param_i_get(cfg_spectrum_fill_gradient) || theme_shape.deck) &&   /* deck: always */
           !theme_shape.mono;                                                                           /* 1bit: bars */
}

/* ---- LVGL drawing: the walkie's bandscope, the main spectrum without planes, the markers ---- */

static lv_coord_t level_y(float db, float min, float max, lv_coord_t top, lv_coord_t h) {
    float v = (db - min) / (max - min);

    return top + (1.0f - v) * h;
}

static void vline(lv_draw_ctx_t *ctx, const lv_draw_line_dsc_t *dsc, lv_coord_t x, lv_coord_t top, lv_coord_t h) {
    lv_point_t a = {x, top}, b = {x, top + h};

    lv_draw_line(ctx, dsc, &a, &b);
}

static void shade(lv_draw_ctx_t *ctx, const lv_draw_rect_dsc_t *dsc, lv_coord_t x1, lv_coord_t x2, lv_coord_t top, lv_coord_t h) {
    lv_area_t a = {x1, top, x2, top + h};

    lv_draw_rect(ctx, dsc, &a);
}

/* Two decoder tones (audio Hz) as lines, on the passband's side */
static void tone_lines(lv_draw_ctx_t *ctx, const lv_draw_line_dsc_t *dsc, lv_coord_t vfo_x, lv_coord_t top, lv_coord_t h,
                       lv_coord_t w, int32_t span, int32_t lo, int32_t hi) {
    vline(ctx, dsc, vfo_x + w * (pass_lo > 0 ? lo : -lo) / span, top, h);
    vline(ctx, dsc, vfo_x + w * (pass_hi > 0 ? hi : -hi) / span, top, h);
}

/* The trace and its peaks: a line through the points, or a column under each when filled */
static void trace_draw(lv_draw_ctx_t *ctx, lv_obj_t *o, lv_coord_t shift, const lv_draw_line_dsc_t *body) {
    lv_coord_t         x0 = o->coords.x1 + shift, top = o->coords.y1;
    lv_coord_t         w = lv_obj_get_width(o), h = lv_obj_get_height(o);
    bool               filled = params.spectrum_filled.x, grad = fill_grad(), over = fill_own() || grad;
    bool               peaks = params.spectrum_peak.x && !data_tx;
    /* From the foot of the first point: from the left corner, a shifted trace (Fixed mode) had a
     * slope across the side tuned away from */
    lv_point_t         prev = {x0, top + h}, prev_peak = prev, prev_over = {0, 0};
    lv_draw_line_dsc_t peak_dsc, over_dsc;
    float              min, max;

    /* LVGL caches a gradient by its descriptor's address and size, not its colours.
     * Static: on the stack it could take another widget's (the clock box came out with
     * this fill); and new colours empty the cache, or the old columns stay */
    static lv_draw_rect_dsc_t grad_dsc;
    lv_color_t                fill = fill_color(), bg = lv_obj_get_style_bg_color(o, LV_PART_MAIN);

    if (grad && (grad_dsc.bg_grad.stops[0].color.full != fill.full || grad_dsc.bg_grad.stops[1].color.full != bg.full)) {
        lv_gradient_set_cache_size(LV_GRAD_CACHE_DEF_SIZE);    /* empty (free_cache would turn it off) */
    }
    levels(&min, &max);
    lv_draw_line_dsc_init(&peak_dsc);
    peak_dsc.color = theme_shape.mono ? TC(line) : TC(off);   /* 1bit: a line above the bars */
    peak_dsc.width = 1;
    lv_draw_line_dsc_init(&over_dsc);
    over_dsc.color = trace_color();
    over_dsc.width = 1;
    lv_draw_rect_dsc_init(&grad_dsc);
    grad_dsc.bg_grad.dir = LV_GRAD_DIR_VER;
    grad_dsc.bg_grad.stops[0].color = fill;
    grad_dsc.bg_grad.stops[1].color = bg;

    for (int i = 0; i < POINTS; i++) {
        lv_coord_t x = x0 + i * w / POINTS;
        lv_point_t p = {x, level_y(trace_db[i], min, max, top, h)};

        if (peaks) {
            lv_point_t pk = {x, level_y(peak_db[i], min, max, top, h)};

            lv_draw_line(ctx, &peak_dsc, &pk, &prev_peak);
            prev_peak = pk;
        }
        if (!filled) {
            lv_draw_line(ctx, body, &p, &prev);
            prev = p;
        } else if (grad) {
            lv_area_t col = {x, p.y, x, top + h};

            lv_draw_rect(ctx, &grad_dsc, &col);
        } else {
            lv_point_t foot = {x, top + h};

            lv_draw_line(ctx, body, &p, &foot);
        }
        if (over) {
            if (i) lv_draw_line(ctx, &over_dsc, &prev_over, &p);
            prev_over = p;
        }
    }
}

/* Fixed mode: on the side tuned away from, the trace last received there, a dimmed line */
static void kept_draw(lv_draw_ctx_t *ctx, lv_obj_t *o) {
    lv_coord_t         top = o->coords.y1;
    lv_coord_t         w = lv_obj_get_width(o), h = lv_obj_get_height(o);
    lv_draw_line_dsc_t dsc;
    lv_point_t         prev = {0, 0};
    bool               joined = false;
    float              min, max;
    int                from, to;

    if (!kept_side(&from, &to)) {
        return;
    }
    levels(&min, &max);
    lv_draw_line_dsc_init(&dsc);
    dsc.color = kept_color(trace_color(), lv_obj_get_style_bg_color(o, LV_PART_MAIN));
    dsc.width = 1;
    if (theme_shape.mono) {     /* 1bit: dotted (the mono layer's stipple), no half tone */
        dsc.color = TC(line);
        dsc.opa = LV_OPA_40;
    }
    for (int i = from; i < to; i++) {
        if (isnan(kept_db[i])) {
            joined = false;
            continue;
        }
        lv_point_t p = {o->coords.x1 + i * w / POINTS, level_y(kept_db[i], min, max, top, h)};

        if (joined) {
            lv_draw_line(ctx, &dsc, &prev, &p);
        }
        prev = p;
        joined = true;
    }
}

/* The trace is on the primary plane: the main screen's spectrum in direct mode, not the walkie's */
static bool on_plane(lv_obj_t *o) {
    return direct && o == obj;
}

void spectrum_draw_cb(lv_event_t *e) {
    lv_obj_t          *o = lv_event_get_target(e);
    lv_draw_ctx_t     *ctx = lv_event_get_draw_ctx(e);
    lv_coord_t         top = o->coords.y1;
    lv_coord_t         w = lv_obj_get_width(o), h = lv_obj_get_height(o);
    lv_coord_t         shift = trace_px(w);
    lv_coord_t         vfo_x = o->coords.x1 + markers_px(w) + w / 2;
    int32_t            span = SPAN_HZ / zoom;
    lv_draw_line_dsc_t line;
    lv_draw_rect_dsc_t band;

    lv_draw_line_dsc_init(&line);
    line.color = fill_color();
    line.width = 1;
    if (!on_plane(o)) {
        trace_draw(ctx, o, shift, &line);
        kept_draw(ctx, o);
    }

    /* The passband, + 1 px each edge */
    int32_t fc = theme_filter_color(param_i_get(cfg_filter_color));

    lv_draw_rect_dsc_init(&band);
    band.bg_color = fc >= 0 ? lv_color_hex(fc) : theme_shape.deck ? TC(vfo_bg) : bg_color;   /* deck: shaded on the black */
    band.bg_opa = LV_OPA_50;
    lv_coord_t pass_x1 = vfo_x + (int32_t)((float)(w * pass_lo) / span + 1.0f);
    lv_coord_t pass_x2 = vfo_x + (int32_t)((float)(w * pass_hi) / span + 1.0f);

    if (theme_shape.mono) {     /* 1bit: its two edges, no shade over the bars */
        vline(ctx, &line, pass_x1, top, h);
        vline(ctx, &line, pass_x2, top, h);
    } else {
        shade(ctx, &band, pass_x1, pass_x2, top, h);
    }

    /* notch_hz already carries the sideband's sign: no second flip, or LSB lands on the USB side */
    if (notch_on) {
        int32_t a = notch_hz - notch_half, b = notch_hz + notch_half;

        band.bg_color = TC(text);
        if (theme_shape.mono) band.bg_opa = LV_OPA_30;   /* a stippled band */
        shade(ctx, &band, vfo_x + w * LV_MIN(a, b) / span, vfo_x + w * LV_MAX(a, b) / span, top, h);
    }

    /* The decoders' mark and space: RTTY as set, NAVTEX 1000 Hz +-85, WEFAX 1500 Hz +-425 */
    if (rtty_get_state() != RTTY_OFF) {
        tone_lines(ctx, &line, vfo_x, top, h, w, span,
                   params.rtty_center - params.rtty_shift / 2, params.rtty_center + params.rtty_shift / 2);
    }
    if (dialog_navtex_is_active()) {
        tone_lines(ctx, &line, vfo_x, top, h, w, span, 915, 1085);
    }
    if (dialog_wefax_is_active()) {
        tone_lines(ctx, &line, vfo_x, top, h, w, span, 1075, 1925);
    }

    /* The VFO line as the waterfall's, red while recording; none in CW */
    line.width = 3;
    line.color = recorder_is_on() ? TC(err_strong) : TC(vfo_line);
    if (vfo_line_on && theme_shape.mono) {     /* 1bit: 1 px ink in a paper halo, readable over the bars */
        line.color = TC(panel);
        vline(ctx, &line, vfo_x, top, h);
        line.width = 1;
        line.color = TC(vfo_line);
        vline(ctx, &line, vfo_x, top, h);
    } else if (vfo_line_on) {
        vline(ctx, &line, vfo_x, top, h);
    }
}

/* ---- Direct mode: the trace drawn into strip, antialiased ---- */

static inline uint32_t blend_xrgb(uint32_t dst, uint32_t src, uint8_t a) {
    uint32_t rb = ((dst & 0x00FF00FFu) * (255u - a) + (src & 0x00FF00FFu) * a + 0x00800080u);
    uint32_t g  = ((dst & 0x0000FF00u) * (255u - a) + (src & 0x0000FF00u) * a + 0x00008000u);
    return ((rb >> 8) & 0x00FF00FFu) | ((g >> 8) & 0x0000FF00u) | 0xff000000u;
}

/* A landscape pixel of the strip, with coverage c (0..1) */
static void plot(int x, int y, float c, uint32_t color) {
    if (x < 0 || x >= POINTS || y < 0 || y >= strip_h || c <= 0.0f) {
        return;
    }
    uint32_t *p = &strip[(POINTS - 1 - x) * strip_h + y];

    *p = blend_xrgb(*p, color, c >= 1.0f ? 255 : (uint8_t)(c * 255.0f + 0.5f));
}

/* Antialiased line, Wu's algorithm (upstream spectrum_wu_line, landscape) */
static void wu_line(float x0, float y0, float x1, float y1, uint32_t color) {
    bool  steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    float t;

    if (steep) {
        t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    float dx = x1 - x0;
    float gradient = (dx == 0.0f) ? 1.0f : (y1 - y0) / dx;
    int   xa = (int)roundf(x0), xb = (int)roundf(x1);
    float y = y0 + gradient * (xa - x0);

    for (int x = xa; x <= xb; x++) {
        int   yi = (int)floorf(y);
        float f = y - floorf(y);

        if (steep) {
            plot(yi, x, 1.0f - f, color);
            plot(yi + 1, x, f, color);
        } else {
            plot(x, yi, 1.0f - f, color);
            plot(x, yi + 1, f, color);
        }
        y += gradient;
    }
}

/* 1bit: a line of whole pixels (Bresenham), every pixel or every other one (dotted) */
static void mono_line(int x0, int y0, int x1, int y1, uint32_t color, bool dotted) {
    int dx = LV_ABS(x1 - x0), dy = -LV_ABS(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;

    for (;;) {
        if (!dotted || ((x0 + y0) & 1) == 0) plot(x0, y0, 1.0f, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;

        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Points from..to-1 of db, joined where they are not NAN; dotted: 1bit's kept trace */
static void polyline(const float *db, int from, int to, float min, float max, lv_coord_t shift, uint32_t color,
                     bool dotted) {
    float px = 0, py = 0;
    bool  joined = false;

    for (int i = from; i < to; i++) {
        if (isnan(db[i])) {
            joined = false;
            continue;
        }
        float x = i + shift;
        float y = (1.0f - (db[i] - min) / (max - min)) * strip_h;

        y = LV_CLAMP(0.0f, y, (float)strip_h);
        if (joined && theme_shape.mono) {
            mono_line(lroundf(px), lroundf(py), lroundf(x), lroundf(y), color, dotted);
        } else if (joined) {
            wu_line(px, py, x, y, color);
        }
        px = x;
        py = y;
        joined = true;
    }
}

/* The markers drawn by LVGL: redrawn only when one of them changes */
static void markers_check(void) {
    int32_t now[sizeof(drawn_markers) / sizeof(drawn_markers[0])] = {
        markers_px(POINTS), pass_lo, pass_hi, notch_on, notch_hz, notch_half,
        rtty_get_state(), params.rtty_center, params.rtty_shift,
        dialog_navtex_is_active() | dialog_wefax_is_active() << 1, vfo_line_on, recorder_is_on()
    };

    if (memcmp(now, drawn_markers, sizeof(now))) {
        memcpy(drawn_markers, now, sizeof(now));
        lv_obj_invalidate(obj);
    }
}

/* Filled: a column under each point, flat or fading to the panel */
static void fill_columns(float min, float max, lv_coord_t shift) {
    lv_color_t fc = fill_color(), pc = TC(panel);
    uint32_t   flat = fc.full | 0xff000000u;
    bool       grad = fill_grad();
    int32_t    h = strip_h;

    for (int i = 0; i < POINTS; i++) {
        int x = i + shift;

        if (x < 0 || x >= POINTS) {
            continue;
        }
        int       top = (1.0f - (trace_db[i] - min) / (max - min)) * h;
        uint32_t *col = &strip[(POINTS - 1 - x) * h];

        top = LV_MAX(top, 0);
        for (int y = top; y < h; y++) {
            col[y] = grad ? lv_color_mix(fc, pc, (h - y) * 255 / (h - top)).full | 0xff000000u : flat;
        }
    }
}

static void direct_render(void) {
    lv_coord_t h = lv_obj_get_height(obj);
    float      min, max;

    markers_check();
    /* Spectrum off: transparent, under the top bars */
    if (!lv_obj_is_visible(obj) || h <= 0 || lv_obj_get_style_opa(obj, 0) < LV_OPA_MIN) {
        return;
    }
    if (h != strip_h) {
        free(strip);
        strip = malloc(POINTS * h * sizeof(uint32_t));
        strip_h = strip ? h : 0;
        if (!strip) {
            return;
        }
    }
    uint32_t bg = TC(panel).full | 0xff000000u;
    uint32_t line = trace_color().full | 0xff000000u;

    for (int32_t i = 0; i < POINTS * h; i++) {
        strip[i] = bg;
    }
    levels(&min, &max);
    if (max <= min) {
        max = min + 1.0f;
    }
    lv_coord_t shift = trace_px(POINTS);
    int        from, to;

    pthread_mutex_lock(&data_mux);
    if (params.spectrum_peak.x && !data_tx) {
        polyline(peak_db, 0, POINTS, min, max, shift, theme_shape.mono ? line : TC(off).full, false);
    }
    if (params.spectrum_filled.x) {
        fill_columns(min, max, shift);
    }
    if (!params.spectrum_filled.x || fill_own() || fill_grad()) {
        polyline(trace_db, 0, POINTS, min, max, shift, line, false);
    }
    pthread_mutex_unlock(&data_mux);
    if (kept_side(&from, &to)) {   /* 1bit: dotted ink, no half tone */
        polyline(kept_db, from, to, min, max, 0, theme_shape.mono ? line : kept_color(trace_color(), TC(panel)).full,
                 true);
    }

#if USE_DRM
    drm_wf_present(strip, obj->coords.y1, h);
#endif
}

static void spectrum_refresh(void *data) {
    keep_trace();
    if (!direct) {
        lv_obj_invalidate(obj);
    } else if (lv_obj_get_screen(obj) == lv_scr_act()) {    /* not under the walkie screen */
        direct_render();
    }
}

/* ---- Settings ---- */

static void on_zoom_changed(Subject *subj, void *user_data) {
    zoom = (uint8_t)subject_i_get((SubjectInt *)subj);
    spectrum_clear();
}

/* The modes that listen below the VFO */
static bool below(radio_mode_t mode) {
    return mode == radio_mode_lsb || mode == radio_mode_lsb_dig || mode == radio_mode_cwr;
}

static void on_filter_change(Subject *subj, void *user_data) {
    int32_t      low = subject_i_get((SubjectInt *)cfg_cur_filter_low);
    int32_t      high = subject_i_get((SubjectInt *)cfg_cur_filter_high);
    radio_mode_t mode = subject_i_get((SubjectInt *)cfg_cur_mode);

    if (mode == radio_mode_am || mode == radio_mode_nfm || mode == radio_mode_sam || mode == radio_mode_wfm) {
        pass_lo = -high;        /* both sidebands */
        pass_hi = high;
    } else if (below(mode)) {
        pass_lo = -high;
        pass_hi = -low;
    } else {
        pass_lo = low;
        pass_hi = high;
    }
}

/* The manual notch: shown only when DNF is on, not automatic, and the mode has a
 * sideband; centred on its frequency (upstream 8e4f1d6) */
static void on_notch_change(Subject *subj, void *user_data) {
    /* Redrawn on the next loop: the change may come from another thread (CAT) */
    lv_async_call((void (*)(void *))lv_obj_invalidate, obj);

    radio_mode_t mode = subject_i_get((SubjectInt *)cfg_cur_mode);

    notch_on = subject_i_get((SubjectInt *)cfg_dnf) && !subject_i_get((SubjectInt *)cfg_dnf_auto) &&
               mode != radio_mode_am && mode != radio_mode_nfm && mode != radio_mode_sam && mode != radio_mode_wfm;
    if (notch_on) {
        notch_half = subject_i_get((SubjectInt *)cfg_dnf_width);
        notch_hz = subject_i_get((SubjectInt *)cfg_dnf_center);
        if (below(mode)) notch_hz = -notch_hz;
    }
}

static void on_mode_change(Subject *subj, void *user_data) {
    radio_mode_t mode = subject_i_get((SubjectInt *)subj);

    vfo_line_on = mode != radio_mode_cw && mode != radio_mode_cwr;
}

static void on_lo_offset_change(Subject *subj, void *user_data) {
    lo_offset = subject_i_get((SubjectInt *)subj);
    kept_settle();
}

static void on_if_shift_change(Subject *subj, void *user_data) {
    if_shift = subject_i_get((SubjectInt *)subj);
    kept_settle();
}

/* Manual levels follow the band's settings; automatic ones come from the DSP */
static void on_levels_change(Subject *subj, void *user_data) {
    if (!param_i_get(cfg_auto_level_enabled)) {
        level_min = param_i_get(cfg_band_grid_min);
        level_max = param_i_get(cfg_band_grid_max);
    }
}

static void on_vfo_change(Subject *subj, void *user_data) {
    vfo_hz = cparam_i_get(cfg_fg_freq);
    kept_settle();
    scheduler_put_coalesced(spectrum_refresh);
}

static void on_rit_change(Subject *subj, void *user_data) {
    rit_hz = param_i_get(cfg_rit);
}

lv_obj_t *spectrum_init(lv_obj_t *parent) {
    if (!obj) pthread_mutex_init(&data_mux, NULL);   /* the DSP thread may hold it on a theme change */

    obj = lv_obj_create(parent);
    lv_obj_add_style(obj, &spectrum_style, 0);
#if USE_DRM
    direct = drm_planes();      /* also when off: it can be shown live (main_screen.c) */
#endif
    if (direct) {
        /* Transparent, then the markers over the primary plane */
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(obj, direct_hole_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    }
    lv_obj_add_event_cb(obj, spectrum_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    subject_subscribe_and_notify((Subject *)cfg_mode_zoom, on_zoom_changed, NULL);   /* clears the trace */
    subject_subscribe((Subject *)cfg_cur_filter_low, on_filter_change, NULL);
    subject_subscribe((Subject *)cfg_cur_filter_high, on_filter_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_cur_mode, on_filter_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_cur_mode, on_mode_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_mode_lo_offset, on_lo_offset_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_band_if_shift, on_if_shift_change, NULL);
    subject_subscribe((Subject *)cfg_auto_level_enabled, on_levels_change, NULL);
    subject_subscribe((Subject *)cfg_band_grid_min, on_levels_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_band_grid_max, on_levels_change, NULL);
    subject_subscribe((Subject *)cfg_cur_mode, on_notch_change, NULL);
    subject_subscribe((Subject *)cfg_dnf, on_notch_change, NULL);
    subject_subscribe((Subject *)cfg_dnf_auto, on_notch_change, NULL);
    subject_subscribe((Subject *)cfg_dnf_center, on_notch_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_dnf_width, on_notch_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_fg_freq, on_vfo_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_rit, on_rit_change, NULL);
    return obj;
}

/* The peaks follow the base when it moves: whole points, the rest added to the next move */
static void shift_peaks(int32_t df) {
    uint64_t now = get_time();
    uint16_t bin_hz = SPAN_HZ / POINTS / zoom;
    int32_t  n;

    df += peak_rest_hz;
    n = (df + bin_hz / 2) / bin_hz;
    peak_rest_hz = df - n * bin_hz;
    if (n == 0) {
        return;
    }
    int32_t keep = LV_MAX(POINTS - LV_ABS(n), 0);
    int32_t from = n > 0 ? n : 0, to = n > 0 ? 0 : -n;      /* where the kept ones are, and go */
    int32_t gone = n > 0 ? keep : 0;                        /* the first of those that come in empty */

    memmove(&peak_db[to], &peak_db[from], keep * sizeof(peak_db[0]));
    memmove(&peak_at[to], &peak_at[from], keep * sizeof(peak_at[0]));
    for (int32_t i = gone; i < gone + POINTS - keep; i++) {
        peak_db[i] = S_MIN;
        peak_at[i] = now;
    }
}

void spectrum_data(float *db, uint16_t size, bool tx, uint32_t base_freq, uint8_t fft_dec) {
    uint64_t now = get_time();

    if (base_freq != base_lo_hz) {
        int32_t df = base_freq - base_lo_hz;

        base_lo_hz = base_freq;
        shift_peaks(df);
    }

    pthread_mutex_lock(&data_mux);
    data_tx = tx;
    if (kept_wait) {
        kept_wait--;
    } else {
        kept_new = true;
    }
    memcpy(trace_db, db, size * sizeof(float));
    if (params.spectrum_peak.x && !tx) {
        /* A peak holds for its time, then sinks; a new zoom starts them again */
        for (uint16_t i = 0; i < size; i++) {
            if (trace_db[i] > peak_db[i] || fft_dec != last_fft_dec) {
                peak_db[i] = trace_db[i];
                peak_at[i] = now;
            } else if (now - peak_at[i] > (int)params.spectrum_peak_hold.x * 1000) {
                peak_db[i] -= params.spectrum_peak_speed.x * 0.1f;
            }
        }
    }
    last_fft_dec = fft_dec;
    pthread_mutex_unlock(&data_mux);
    scheduler_put_coalesced(spectrum_refresh);
}

int32_t spectrum_span(float *vfo_at) {
    *vfo_at = 0.5f + (float)markers_px(POINTS) / POINTS;
    return SPAN_HZ / zoom;
}

void spectrum_min_max_reset() {
    level_min = TX_MIN;
    level_max = TX_MAX;
    on_levels_change(NULL, NULL);
}

/* Auto levels: the DSP's floor and top, less the offset set */
static void auto_level(float *level, float db) {
    if (param_i_get(cfg_auto_level_enabled)) {
        *level = db - param_f_get(cfg_auto_level_offset);
    }
}

void spectrum_update_max(float db) {
    auto_level(&level_max, db);
}

void spectrum_update_min(float db) {
    auto_level(&level_min, db);
}

void spectrum_clear() {
    uint64_t now = get_time();

    spectrum_min_max_reset();
    peak_rest_hz = 0;
    for (int i = 0; i < POINTS; i++) {
        trace_db[i] = S_MIN;
        peak_db[i] = S_MIN;
        peak_at[i] = now;
    }
}

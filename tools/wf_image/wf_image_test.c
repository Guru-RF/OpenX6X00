/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the waterfall without planes (src/waterfall_view.c's image path, #206): the PC build, a
 *  panel on fbdev, a single DRM plane. There the waterfall is an LVGL image scrolled as the planes' picture is: new
 *  lines move it down and only they are computed, a tune slides it by whole pixels and only the strip coming in is.
 *  Its scroll has no timer: a step is taken each time the image is drawn (image_drawn_cb()).
 *
 *  The real waterfall_view.c, included whole, on the PC build's LVGL: an 800 x 480 display, the waterfall made by
 *  waterfall_init() and waterfall_set_height(), its rows from waterfall_data() as the DSP thread gives them (each of
 *  one level, its colour in the test's palette), the GUI loop run as gui_main.c runs it (the jobs, then LVGL's timers,
 *  the clock stepped by LVGL's refresh period). What the waterfall reaches of the rest of the GUI the test stands in
 *  for, and nothing is over the waterfall: band_info_init() makes no band-plan strip here. Checked, Centered mode,
 *  Smooth scroll on: a tune from rest slides the picture to the new centre with no new line, to its end (the last
 *  steps are under a pixel). In the GUI the strip at the waterfall's top is drawn again at every VFO change, and the
 *  image under it with it, which took the slide's first step; image_redraw() did not draw it, and with nothing over
 *  the waterfall the slide waited for the next line. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "waterfall_view.c"

#include <stdarg.h>
#include <stdio.h>

#define W       800
#define H       480
#define WF_H    200         /* the waterfall's height */
#define STEP    33          /* ms a loop: LVGL's refresh period */
#define FREQ    7074000

/* ---- What the waterfall calls of the rest of the GUI, stood in for ---- */

params_t        params;
theme_shape_t   theme_shape;
bool            theme_amber;
const uint32_t *wf_palette;
const uint32_t  wf_palette_legacy[256], wf_palette_gauss[256];
lv_style_t      waterfall_style;
int             buttons_h = 60;
lv_font_t       sony_16;
uint32_t        lv_tick_mono_now;   /* LVGL's clock (lv_tick_mono.h), stepped by loop() */

static uint32_t pal[256];           /* level v: its own colour */
static int32_t  fg_freq = FREQ;

/* The parameters: one stand-in object each, by its address */
static char     param[64];
#define P(n) (void *)&param[n]

ParamInt         *cfg_wf_mode = P(0), *cfg_wf_3d = P(1), *cfg_waterfall_speed = P(2), *cfg_mode_zoom = P(3),
                 *cfg_band_if_shift = P(4), *cfg_key_tone = P(5), *cfg_wf_palette = P(6), *cfg_filter_color = P(7),
                 *cfg_auto_level_enabled = P(8), *cfg_band_grid_min = P(9), *cfg_band_grid_max = P(10),
                 *cfg_waterfall_ref = P(11), *cfg_wf_fixed_jump = P(12), *cfg_wf_fixed_after = P(13),
                 *cfg_wf_3d_colors = P(14), *cfg_wf_3d_depth = P(15), *cfg_wf_3d_tilt = P(16),
                 *cfg_wf_3d_floor = P(17), *cfg_wf_3d_peak = P(18), *cfg_wf_3d_glide = P(19),
                 *cfg_wf_3d_range = P(20);
ComputedParamInt *cfg_fg_freq = P(30), *cfg_cur_filter_low = P(31), *cfg_cur_filter_high = P(32),
                 *cfg_cur_mode = P(33), *cfg_mode_lo_offset = P(34);

int32_t param_i_get(const ParamInt *p) {
    if (p == cfg_waterfall_speed) return 3;
    if (p == cfg_mode_zoom) return 1;
    if (p == cfg_band_grid_max) return 255;     /* with the minimum 0: a row's level is its colour index */
    return 0;
}

float param_f_get(const ParamFloat *p) {
    (void)p;
    return 0.0f;
}

int32_t cparam_i_get(const ComputedParamInt *p) {
    return p == cfg_fg_freq ? fg_freq : 0;
}

int32_t subject_i_get(SubjectInt *subj) {
    if (subj == (SubjectInt *)cfg_fg_freq) return fg_freq;
    if (subj == (SubjectInt *)cfg_cur_filter_high) return 2700;
    if (subj == (SubjectInt *)cfg_mode_zoom) return 1;
    return 0;
}

/* The observers of the VFO, run by tune() as the parameters run them: the plain ones at once, the delayed ones from
 * the GUI loop */
static observer_cb fg_now, fg_later;

static void observe(Subject *subj, observer_cb fn, bool delayed, bool notify) {
    if (subj == (Subject *)cfg_fg_freq) {
        *(delayed ? &fg_later : &fg_now) = fn;
    }
    if (notify) {
        fn(subj, NULL);
    }
}

Observer *subject_subscribe(Subject *subj, observer_cb fn, void *user_data) {
    (void)user_data;
    observe(subj, fn, false, false);
    return NULL;
}

Observer *subject_subscribe_and_notify(Subject *subj, observer_cb fn, void *user_data) {
    (void)user_data;
    observe(subj, fn, false, true);
    return NULL;
}

ObserverDelayed *subject_subscribe_delayed(Subject *subj, observer_cb fn, void *user_data) {
    (void)user_data;
    observe(subj, fn, true, false);
    return NULL;
}

ObserverDelayed *subject_subscribe_delayed_and_notify(Subject *subj, observer_cb fn, void *user_data) {
    (void)user_data;
    observe(subj, fn, true, true);
    return NULL;
}

/* The jobs for the GUI thread, run by loop() as scheduler_work() runs them */
static scheduler_fn_t jobs[8];
static int            njobs;

bool scheduler_put_coalesced(scheduler_fn_t fn) {
    for (int i = 0; i < njobs; i++) {
        if (jobs[i] == fn) return true;
    }
    jobs[njobs++] = fn;
    return true;
}

uint64_t get_time() {
    return lv_tick_mono_now;
}

const uint32_t *theme_waterfall(void) {
    return pal;
}

const uint8_t *theme_wf_density(void) {
    static uint8_t d[256];

    return d;
}

void theme_ramp_palette(const uint32_t *stops, int n, int steps, uint32_t *out) {
    (void)stops, (void)n, (void)steps, (void)out;
}

uint32_t theme_hex(theme_color_t c) {
    (void)c;
    return 0xffa000;
}

int32_t theme_filter_color(int32_t i) {
    (void)i;
    return -1;
}

bool backlight_seen() {
    return true;
}

bool walkie_on(void) {
    return false;
}

void freq_scale_lines_hide(bool hide) {
    (void)hide;
}

int freq_scale_refs(int32_t *x, int32_t *hz, int max) {
    (void)x, (void)hz, (void)max;
    return 0;
}

bool buttons_hidden() {
    return false;
}

void split_freq(int32_t freq, uint16_t *mhz, uint16_t *khz, uint16_t *hz) {
    *mhz = freq / 1000000;
    *khz = freq / 1000 % 1000;
    *hz = freq % 1000;
}

lv_obj_t *band_info_init(lv_obj_t *waterfall) {
    (void)waterfall;
    return NULL;
}

int radio_cw_tone_dir(radio_mode_t mode) {
    (void)mode;
    return 1;
}

/* ---- The checks ---- */

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    (void)a, (void)px;
    lv_disp_flush_ready(drv);
}

/* The GUI loop for ms, as gui_main.c's: the jobs, then LVGL's timers (the screen's refresh among them) */
static void loop(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += STEP) {
        int n = njobs;

        njobs = 0;
        for (int i = 0; i < n; i++) {
            jobs[i](NULL);
        }
        lv_tick_mono_now += STEP;
        lv_timer_handler();
    }
}

/* A row from the DSP thread, every level v: the X6200's way, with the frequency the base was tuned to */
static uint8_t level;

static void row(void) {
    float db[WATERFALL_NFFT];

    level = level % 250 + 1;
    for (int i = 0; i < WATERFALL_NFFT; i++) db[i] = level;
    waterfall_data(db, WATERFALL_NFFT, false, radio_center_freq + mode_lo_offset, 100000);
}

/* The VFO moved by hz, as the parameters tell it */
static void tune(int32_t hz) {
    fg_freq += hz;
    fg_now((Subject *)cfg_fg_freq, NULL);
    fg_later((Subject *)cfg_fg_freq, NULL);
}

/* Every line of the picture all the colour of its row as the rows are now (age y on line y): the first that is not */
static int32_t bad_line(int32_t *x_bad) {
    uint16_t top = __atomic_load_n(&newest, __ATOMIC_ACQUIRE);

    for (int32_t y = 0; y < height; y++) {
        const lv_color_t *px = (const lv_color_t *)frame->data + (size_t)y * WIDTH;
        uint32_t          want = pal[row_from(top, y)->values[DATA_AT]];

        for (int32_t x = 0; x < WIDTH; x++) {
            if ((px[x].full | 0xff000000) != (want | 0xff000000)) {
                *x_bad = x;
                return y;
            }
        }
    }
    return -1;
}

static void picture_is_rows(const char *what) {
    int32_t x = 0, y = bad_line(&x);

    check(y < 0, "%s: each line the row of its age", what);
    if (y >= 0) {
        uint16_t top = newest;

        printf("        line %d, x %d: the colour of level %d where its row is level %d (rows at the top: %d %d %d)\n",
               y, x, (int)(((const lv_color_t *)frame->data)[(size_t)y * WIDTH + x].full >> 16 & 0xff),
               row_from(top, y)->values[DATA_AT], row_from(top, 0)->values[DATA_AT],
               row_from(top, 1)->values[DATA_AT], row_from(top, 2)->values[DATA_AT]);
    }
}

/* Till the picture's centre is on the VFO and the image within a pixel of it, no new line meanwhile: ms, -1 never */
static int32_t slide_ms(uint32_t limit) {
    uint32_t span = shown_span();

    for (uint32_t t = 0; t <= limit; t += STEP) {
        if (wf_center_freq == radio_center_freq && LV_ABS(wfp_center - wf_center_freq) * WIDTH < (int32_t)span) {
            return t;
        }
        loop(STEP);
    }
    return -1;
}

int main(void) {
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t         px[W * H];     /* as gui_main.c's, the whole screen: the image drawn in one piece */
    static lv_disp_drv_t      drv;

    for (int v = 0; v < 256; v++) pal[v] = 0xff000000 | (uint32_t)v << 16 | (uint32_t)(255 - v) << 8 | 0x40;
    params.waterfall_smooth_scroll.x = true;
    params.waterfall_center_line.x   = true;

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, px, NULL, W * H);
    lv_disp_drv_init(&drv);
    drv.hor_res  = W;
    drv.ver_res  = H;
    drv.flush_cb = flush;
    drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&drv);

    lv_obj_t *wf = waterfall_init(lv_scr_act());

    lv_obj_set_size(wf, W, WF_H);
    lv_obj_set_pos(wf, 0, H - WF_H);
    waterfall_set_height(WF_H);
    check(!direct && img && height == WF_H, "no planes: the waterfall an image, %d lines", height);

    for (int i = 0; i < WF_H; i++) row();
    loop(300);
    picture_is_rows("at rest, the waterfall full");

    printf("A tune of 500 Hz from rest, no new line meanwhile\n");
    tune(500);
    int32_t ms = slide_ms(1500);

    check(ms >= 0, "the picture slid to the new centre (%d ms; the image %d Hz off it, the centre %d Hz)", ms,
          wfp_center - radio_center_freq, wf_center_freq - radio_center_freq);
    picture_is_rows("after the slide");

    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}

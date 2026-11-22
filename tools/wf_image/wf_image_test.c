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
 *  one level, its colour in the test's palette: a line shows which row it is), the GUI loop run as gui_main.c runs it
 *  (the jobs, then LVGL's timers, the clock stepped by LVGL's refresh period). What the waterfall reaches of the rest
 *  of the GUI the test stands in for, and nothing is over the waterfall: band_info_init() makes no band-plan strip
 *  here. Checked, Centered mode, Smooth scroll on:
 *  - a tune from rest: the picture slides to the new centre with no new line, to its end (the last steps are under a
 *    pixel). In the GUI the strip at the waterfall's top is drawn again at every VFO change, and the image under it
 *    with it, which took the slide's first step; image_redraw() did not draw it, and with nothing over the waterfall
 *    the slide waited for the next line
 *  - a row the DSP thread writes after the refresh counted the new ones, before it paints them: the picture is still
 *    the rows as they are, each line its own row, after that refresh and after the next one; also in a slide. Taken
 *    again at each line, the row went in at the top over the one counted, which was never shown, and was shown twice
 *    after the next refresh; in a slide each line's strip came from the row beside it. All of it scrolled down with
 *    the picture till the next full redraw
 *  - a row it writes while a refresh paints the whole picture, half way down (in 1bit, where each pixel painted asks
 *    the theme for its ink: there the test writes it): the picture the rows as they were before it, then with it
 *    after the next refresh. Taken again at each line, the lines under it were each the row above their own
 *  - the filter shade over the picture as the screen shows it (#250): with Shaded part Passband over the passband and
 *    nothing else, with Outside every column but the passband's, at Full and 25 %, Edges the passband's two edges with
 *    either; after the passband moved (a narrower filter, the other sideband, a zoom that puts it partly off the
 *    screen) still so, though only the columns between its old and new edges are drawn again: a 1 px move of an edge
 *    redraws a few columns, not the shade's whole width
 *  - in CW and CW-R, on an X6100 base that sends no frequency with its data (Xiegu's): a station on the dial in
 *    receive and the carrier in TX on the line, the rows placed by the LO offsets as the settings give them for each
 *    base (cfg/lo_offset.c). Xiegu's 1.1.8 centres its frames on the oscillator in TX as in receive. Its 1.1.9 sends
 *    its TX frames as if unzoomed, as the reporter's screens show them at zoom 8 (#191): centred on the oscillator,
 *    the carrier a key tone from their middle, drawn over the zoom's span, so the key tone over the zoom from it;
 *    with the receive offset in TX (up to v0.3.10) the carrier was 7/8 of a key tone below the line in CW, with the
 *    frames on the dial (v0.3.11 to v0.3.13) an eighth of it above. At Speed 1 a line holds frames of TX or of
 *    receive, not of both, there apart. A patched base's rows as before. With an IF shift 1.1.9 is tuned that far
 *    from the dial, its receive frames centred there, its TX ones not: a station on the dial in receive and the
 *    carrier in TX on the line in USB, CW and CW-R, with +1.5 kHz and with the reporter's -3 kHz; placed from where
 *    the base was tuned the TX rows were the IF shift off the line (#191). At Speed 1 a line holds frames of TX or
 *    of receive there too
 *  - another height (waterfall_set_height(): the full-screen waterfall and back, the spectrum's height, #302) keeps
 *    the rows that fit: taller, the rows it had are the top lines, newest first, and nothing is under them; lower,
 *    its newest rows stay; a new row comes in at the top each time. It emptied the waterfall before
 *  - the walkie screen's mini waterfall (waterfall_mini_draw(), #302), 360 x 100 pixels of the same rows: a station
 *    on the dial is a line down its middle and nowhere else; a call with no new row draws nothing; a new row comes
 *    in at the top and the rest moves down a line; another centre draws it all again, the station where it now is
 *  - 3D: the picture is drawn on a thread of its own (d3_job) and shown by the GUI thread once drawn. The image is
 *    then, pixel for pixel, the picture drawn in place in rows as before the thread; after rows and tunes that came
 *    while a picture was being drawn (the thread held up here, 30 ms a picture), the picture of the last of them;
 *    3D switched off while one is being drawn: the 2D waterfall the rows as they are, and the 3D picture that was on
 *    its way is not shown over it later; the waterfall's height changed while one is being drawn (the image is freed
 *    and made again there): the old height's picture dropped, the new one's shown. `make asan` and `make tsan` run
 *    these under the address and the thread sanitizer
 *  See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "waterfall_view.c"
#include "cfg/lo_offset.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

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
                 *cfg_wf_3d_range = P(20), *cfg_filter_shade = P(21), *cfg_filter_shade_where = P(22);
ComputedParamInt *cfg_fg_freq = P(30), *cfg_cur_filter_low = P(31), *cfg_cur_filter_high = P(32),
                 *cfg_cur_mode = P(33), *cfg_mode_lo_offset = P(34), *cfg_mode_tx_lo_offset = P(35);

static int     speed = 3;           /* Settings > Waterfall > Speed */
static int32_t want_3d;             /* Settings > Display > WATERFALL, 3D */
static int32_t shade = 4;           /* Filter shade: Full, the default (#167) */
static int32_t where = FILTER_SHADE_PASSBAND;  /* Shaded part (#250) */
static int32_t filter_high = 2700, cur_mode = radio_mode_lsb, cur_zoom = 1;
static int32_t lo_rx, lo_tx;        /* the LO offsets, in receive and in TX (lo_offsets()) */
static int32_t if_shift_hz;         /* the band's IF shift (if_shift_to()) */

int32_t param_i_get(const ParamInt *p) {
    if (p == cfg_waterfall_speed) return speed;
    if (p == cfg_mode_zoom) return cur_zoom;
    if (p == cfg_band_grid_max) return 255;     /* with the minimum 0: a row's level is its colour index */
    if (p == cfg_filter_shade) return shade;
    if (p == cfg_filter_shade_where) return where;
    if (p == cfg_band_if_shift) return if_shift_hz;
    if (p == cfg_wf_3d) return want_3d;
    if (p == cfg_wf_3d_colors) return D3_WATERFALL;    /* the test's palette; no held peaks, no glide: a picture */
    if (p == cfg_wf_3d_depth) return 40;                /* is of the rows and the view alone, whenever it is drawn */
    if (p == cfg_wf_3d_range) return 40;
    if (p == cfg_wf_3d_tilt) return 3;
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
    if (subj == (SubjectInt *)cfg_cur_filter_high) return filter_high;
    if (subj == (SubjectInt *)cfg_cur_mode) return cur_mode;
    if (subj == (SubjectInt *)cfg_mode_zoom) return cur_zoom;
    if (subj == (SubjectInt *)cfg_mode_lo_offset) return lo_rx;
    if (subj == (SubjectInt *)cfg_mode_tx_lo_offset) return lo_tx;
    if (subj == (SubjectInt *)cfg_band_if_shift) return if_shift_hz;
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

/* The jobs for the GUI thread, run by loop() as scheduler_work() runs them; the 3D thread puts one too */
#define MAX_JOBS 16
static scheduler_fn_t  jobs[MAX_JOBS];
static int             njobs;
static pthread_mutex_t jobs_mx = PTHREAD_MUTEX_INITIALIZER;

bool scheduler_put_coalesced(scheduler_fn_t fn) {
    bool ok = true;

    pthread_mutex_lock(&jobs_mx);
    for (int i = 0; i < njobs; i++) {
        if (jobs[i] == fn) {
            pthread_mutex_unlock(&jobs_mx);
            return true;
        }
    }
    if (njobs < MAX_JOBS) jobs[njobs++] = fn;
    else ok = false;
    pthread_mutex_unlock(&jobs_mx);
    return ok;
}

bool scheduler_put_noargs(scheduler_fn_t fn) {
    bool ok;

    pthread_mutex_lock(&jobs_mx);
    ok = njobs < MAX_JOBS;
    if (ok) jobs[njobs++] = fn;
    pthread_mutex_unlock(&jobs_mx);
    return ok;
}

uint64_t get_time() {
    return lv_tick_mono_now;
}

const uint32_t *theme_waterfall(void) {
    return pal;
}

/* 1bit: a pixel's ink by its level, here odd levels ink and even ones paper (row() alternates them). Asked for each
 * pixel painted: with write_in set, the DSP thread writes a row after so many more, as it may while the GUI paints */
static uint8_t density[256];
static long    write_in;

static void row(void);

const uint8_t *theme_wf_density(void) {
    if (write_in > 0 && --write_in == 0) {
        row();
    }
    return density;
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

/* Asked once for each 3D picture, by the thread that draws it: with slow_3d set the 3D thread is held up there, so
 * a picture is still being drawn when the GUI thread goes on */
static pthread_t gui_thread;
static _Atomic bool slow_3d;

bool buttons_hidden() {
    if (slow_3d && !pthread_equal(pthread_self(), gui_thread)) {
        usleep(30000);
    }
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

/* The screen as LVGL drew it: each area flushed copied to its place */
static lv_color_t screen[W * H];

static void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    int32_t w = lv_area_get_width(a);

    for (int32_t y = a->y1; y <= a->y2; y++) {
        memcpy(&screen[y * W + a->x1], px + (y - a->y1) * w, w * sizeof(lv_color_t));
    }
    lv_disp_flush_ready(drv);
}

/* The GUI loop for ms, as gui_main.c's: the jobs, then LVGL's timers (the screen's refresh among them) */
static void loop(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += STEP) {
        scheduler_fn_t run[MAX_JOBS];

        pthread_mutex_lock(&jobs_mx);
        int n = njobs;

        memcpy(run, jobs, sizeof(run));
        njobs = 0;
        pthread_mutex_unlock(&jobs_mx);
        for (int i = 0; i < n; i++) {
            run[i](NULL);
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

/* The LO offsets as the settings give them for the base and the mode (settings_manager.cpp's lo_offset_compute() and
 * tx_lo_offset_compute()), told to the waterfall */
static void lo_offsets(int32_t rx, int32_t tx) {
    lo_rx = rx;
    lo_tx = tx;
    on_mode_lo_offset_change((Subject *)cfg_mode_lo_offset, NULL);
    on_tx_lo_offset_change((Subject *)cfg_mode_tx_lo_offset, NULL);
}

/* The IF shift set to hz, as its parameter tells it, at rest after it: no rows of the frequency before to drop */
static void if_shift_to(int32_t hz) {
    if_shift_hz = hz;
    on_if_shift_changed((Subject *)cfg_band_if_shift, NULL);
    skip_rows = 0;
    retuned = false;
}

/* A frame of the X6100's base at the zoom z, its spectrum centred `centre` Hz from the dial as the base centres it,
 * with a carrier on the dial; at: the frequency the base sends with it, 0 none (Xiegu's own bases). In TX the levels
 * of the TX scale */
static void x6100_frame_z(bool tx, int32_t centre, uint32_t at, int32_t z) {
    float   db[WATERFALL_NFFT];
    int32_t width   = SPAN_HZ / z;
    int     carrier = WATERFALL_NFFT / 2 - lround((double)centre * WATERFALL_NFFT / width);   /* the nearest bin */

    for (int i = 0; i < WATERFALL_NFFT; i++) {
        db[i] = i != carrier ? (tx ? TX_MIN : 10) : tx ? TX_MAX : 200;
    }
    waterfall_data(db, WATERFALL_NFFT, tx, at, width);
}

/* The same at zoom 1 */
static void x6100_frame(bool tx, int32_t centre, uint32_t at) {
    x6100_frame_z(tx, centre, at, 1);
}

/* A TX frame of Xiegu's 1.1.9 at the zoom z, as the reporter's screens show it (#191): as if unzoomed, the whole
 * 100 kHz centred on the oscillator a key tone from the dial (lo, the receive LO offset), the carrier on the dial;
 * the GUI takes it as the zoom's span, as it does every frame of that base. No frequency with it */
static void x6100_119_tx_frame(int32_t lo, int32_t z) {
    float db[WATERFALL_NFFT];
    int   carrier = WATERFALL_NFFT / 2 - lround((double)lo * WATERFALL_NFFT / SPAN_HZ);

    for (int i = 0; i < WATERFALL_NFFT; i++) {
        db[i] = i != carrier ? TX_MIN : TX_MAX;
    }
    waterfall_data(db, WATERFALL_NFFT, true, 0, SPAN_HZ / z);
}

/* Where the newest row has its carrier, Hz from the dial: the frequency of its highest value */
static int32_t carrier_at(void) {
    const wf_row_t *r = &rows[__atomic_load_n(&newest, __ATOMIC_ACQUIRE)];
    int             top = 0;

    for (int i = 1; i < WATERFALL_NFFT; i++) {
        if (r->values[DATA_AT + i] > r->values[DATA_AT + top]) top = i;
    }
    return (int32_t)r->center_freq - (int32_t)r->width / 2 + top * (int32_t)r->width / WATERFALL_NFFT - fg_freq;
}

/* The newest row's level away from its carrier, near the low edge: its noise */
static uint8_t noise_level(void) {
    return rows[__atomic_load_n(&newest, __ATOMIC_ACQUIRE)].values[DATA_AT + 8];
}

/* The picture's first n lines each the colour of its level in had, the lines under them empty (level 0), at the left
 * edge, the middle and the right edge */
enum { TALL = 300, LOW = 120 };

static uint32_t colour(uint8_t v);

static bool history_is(const uint8_t *had, int n) {
    static const int32_t xs[] = {3, WIDTH / 2 + 40, WIDTH - 4};

    for (int32_t y = 0; y < height; y++) {
        const lv_color_t *px = (const lv_color_t *)frame->data + (size_t)y * WIDTH;

        for (int i = 0; i < 3; i++) {
            if ((px[xs[i]].full | 0xff000000) != colour(y < n ? had[y] : 0)) {
                printf("        line %d, x %d: 0x%06x, not level %d\n", y, xs[i], px[xs[i]].full & 0xffffff,
                       y < n ? had[y] : 0);
                return false;
            }
        }
    }
    return true;
}

/* The mini waterfall's line y (MW pixels): where its strongest pixel is and its level, and the level at the left
 * edge. The test's palette has a level in its red */
enum { MW = 360, MH = 100 };

static lv_color_t mini_px[MW * MH];

static int mini_peak(int y, int *peak, int *edge) {
    int at = 0;

    for (int x = 1; x < MW; x++) {
        if (mini_px[y * MW + x].ch.red > mini_px[y * MW + at].ch.red) at = x;
    }
    *peak = mini_px[y * MW + at].ch.red;
    *edge = mini_px[y * MW].ch.red;
    return at;
}

/* The VFO moved by hz, as the parameters tell it */
static void tune(int32_t hz) {
    fg_freq += hz;
    fg_now((Subject *)cfg_fg_freq, NULL);
    fg_later((Subject *)cfg_fg_freq, NULL);
}

/* A level's colour: the test's palette, 1bit ink or paper */
static uint32_t colour(uint8_t v) {
    if (theme_shape.mono) {
        return (density[v] ? theme_shape.ink : theme_shape.paper) | 0xff000000;
    }
    return pal[v] | 0xff000000;
}

/* Every line of the picture all the colour of its row, the rows as they were with top the newest (age y on line y) */
static void picture_is(uint16_t top, const char *what) {
    for (int32_t y = 0; y < height; y++) {
        const lv_color_t *px = (const lv_color_t *)frame->data + (size_t)y * WIDTH;
        uint8_t           v = row_from(top, y)->values[DATA_AT];

        for (int32_t x = 0; x < WIDTH; x++) {
            if ((px[x].full | 0xff000000) != colour(v)) {
                check(false, "%s: each line the row of its age", what);
                printf("        line %d, x %d: 0x%06x, its row's level %d is 0x%06x (levels at the top: %d %d %d)\n", y,
                       x, px[x].full & 0xffffff, v, colour(v) & 0xffffff, row_from(top, 0)->values[DATA_AT],
                       row_from(top, 1)->values[DATA_AT], row_from(top, 2)->values[DATA_AT]);
                return;
            }
        }
    }
    check(true, "%s: each line the row of its age", what);
}

static void picture_is_rows(const char *what) {
    picture_is(__atomic_load_n(&newest, __ATOMIC_ACQUIRE), what);
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

/* The filter shade on the screen (#250): on the waterfall's middle line, the columns whose pixel is not the image's.
 * Passband: the columns filter_band covers; Outside: all the others; Edges: its first and last, with either.
 * SHADE_DEBUG=1 in the environment prints them, # shaded, . not */
static void shade_is(const char *what) {
    loop(2 * STEP);

    int32_t           y = height / 2, x1 = filter_band->coords.x1, x2 = filter_band->coords.x2;
    const lv_color_t *pic = (const lv_color_t *)frame->data + (size_t)y * WIDTH;
    const lv_color_t *scr = screen + (size_t)(obj->coords.y1 + y) * W;
    int               wrong = 0, first = -1, shaded = 0;

    if (getenv("SHADE_DEBUG")) {
        printf("obj %d,%d img %d,%d band %d-%d out %d-%d\n", obj->coords.x1, obj->coords.y1, img->coords.x1,
               img->coords.y1, x1, x2, out_x1, out_x2);
        for (int32_t x = 0; x < WIDTH; x++) putchar((scr[x].full & 0xffffff) != (pic[x].full & 0xffffff) ? '#' : '.');
        putchar('\n');
    }
    for (int32_t x = 0; x < WIDTH; x++) {
        bool in = x >= x1 && x <= x2;
        bool want = shade == 0 ? x == x1 || x == x2 : where == FILTER_SHADE_OUTSIDE ? !in : in;
        bool got = (scr[x].full & 0xffffff) != (pic[x].full & 0xffffff);

        shaded += got;
        if (got != want && wrong++ == 0) first = x;
    }
    check(wrong == 0, "%s: the passband on columns %d-%d, %d shaded", what, x1, x2, shaded);
    if (wrong) {
        printf("        %d columns wrong, the first %d (SHADE_DEBUG=1 shows them all)\n", wrong, first);
    }
}

/* The columns waiting to be drawn again in the waterfall (LVGL's invalidated areas, all of the waterfall's height) */
static int32_t columns_waiting(void) {
    lv_disp_t *d = lv_disp_get_default();
    int32_t    n = 0;

    for (uint16_t i = 0; i < d->inv_p; i++) {
        if (!d->inv_area_joined[i] && d->inv_areas[i].y2 >= obj->coords.y1) n += lv_area_get_width(&d->inv_areas[i]);
    }
    return n;
}

/* ---- 3D ---- */

/* A row with carriers over a quiet band, the carriers' heights by the row: something for the traces to show */
static uint32_t d3_rows_fed;

static void d3_row(void) {
    float db[WATERFALL_NFFT];

    d3_rows_fed++;
    for (int i = 0; i < WATERFALL_NFFT; i++) {
        db[i] = 10 + (i * 7 + d3_rows_fed) % 5;
        if (i % 97 < 3) db[i] = 120 + (i + d3_rows_fed * 11) % 120;
    }
    waterfall_data(db, WATERFALL_NFFT, false, radio_center_freq + mode_lo_offset, 100000);
}

/* The GUI loop until no 3D picture is being drawn or asked for and the scroll is at rest, the 3D thread given real
 * time meanwhile; false: not within 5 s */
static bool d3_settle(void) {
    for (int i = 0; i < 5000; i++) {
        loop(STEP);
        pthread_mutex_lock(&jobs_mx);
        int n = njobs;

        pthread_mutex_unlock(&jobs_mx);
        if (!d3_busy && !d3_again && !d3_later && !n && wf_center_freq == radio_center_freq - waterfall_view_shift()
            && __atomic_load_n(&new_rows, __ATOMIC_ACQUIRE) == 0) {
            return true;
        }
        usleep(1000);
    }
    return false;
}

/* The image is the 3D picture of the rows and the view now, as drawn in place in rows (d3_draw(), the renderer of
 * before the thread and the turn); lit: some of it is not the background, the traces are there */
static void d3_image_is(const char *what, bool lit) {
    static uint32_t ref[W * H];
    d3_pic_t        pic = {.center = wf_center_freq, .span = shown_span(), .cx = d3_vanish};
    int32_t         ref_x[MAX_D3_REFS] = {0};
    const uint32_t *shown_px = (const uint32_t *)frame->data;
    int32_t         wrong = 0, first = -1, drawn = 0;

    d3_draw(ref, false, &pic, ref_x, 0);
    d3_drawn_buf = NULL;            /* the test drew a picture of its own: the next one whole */
    for (int32_t i = 0; i < WIDTH * height; i++) {
        if (shown_px[i] != ref[i] && wrong++ == 0) first = i;
        drawn += ref[i] != ref[0];
    }
    check(wrong == 0 && (!lit || drawn > 2000), "%s: the image is the picture drawn in place (%d pixels of traces)", what,
          drawn);
    if (wrong) {
        printf("        %d pixels differ, the first at x %d, line %d: 0x%08x, in place 0x%08x\n", wrong, first % WIDTH,
               first / WIDTH, shown_px[first], ref[first]);
    }
}

static void d3_cases(lv_obj_t *wf) {
    printf("3D: the picture from a thread of its own\n");
    want_3d = 1;
    for (int i = 0; i < 60; i++) d3_row();
    waterfall_redraw();
    check(d3_settle(), "the pictures came");
    check(d3_thread_on && !d3_thread_failed, "drawn by the 3D thread (%u pictures)", d3_job.seq);
    d3_image_is("at rest", true);

    printf("3D: rows and tunes while a picture is being drawn\n");
    uint32_t before = d3_job.seq;
    bool     was_busy = false;

    slow_3d = true;
    for (int i = 0; i < 6; i++) {
        d3_row();
        tune(i % 2 ? -700 : 1200);
        loop(STEP);
        was_busy |= d3_busy;
    }
    slow_3d = false;
    check(was_busy, "a picture was being drawn while they came");
    check(d3_settle(), "the pictures came");
    check(d3_job.seq > before + 1, "more than one picture drawn (%u)", d3_job.seq - before);
    d3_image_is("after them", true);

    printf("3D switched off while a picture is being drawn\n");
    for (int i = 0; i < height; i++) row();     /* rows of one level each: picture_is() tells a row by its line */
    check(d3_settle(), "the pictures came");
    slow_3d = true;
    row();
    loop(2 * STEP);
    check(d3_busy, "a picture is being drawn");
    want_3d = 0;
    waterfall_redraw();
    loop(STEP);
    slow_3d = false;
    check(!d3_busy, "the 2D waterfall waited for it");
    picture_is_rows("the 2D waterfall then");
    usleep(50000);                  /* the thread's word that the picture is drawn, if not here yet */
    loop(4 * STEP);
    picture_is_rows("after the 3D picture's hand-over came");

    printf("3D: the waterfall's height changed while a picture is being drawn\n");
    want_3d = 1;
    for (int i = 0; i < 20; i++) d3_row();
    waterfall_redraw();
    check(d3_settle(), "the pictures came");
    slow_3d = true;
    d3_row();
    loop(2 * STEP);
    check(d3_busy, "a picture is being drawn");
    lv_obj_set_size(wf, W, WF_H - 50);
    lv_obj_set_pos(wf, 0, H - (WF_H - 50));
    waterfall_set_height(WF_H - 50);
    slow_3d = false;
    check(!d3_busy && height == WF_H - 50 && frame->header.h == WF_H - 50, "waited for and dropped, the image %d lines",
          frame->header.h);
    for (int i = 0; i < 30; i++) d3_row();
    check(d3_settle(), "the pictures came");
    d3_image_is("at the new height", true);

    want_3d = 0;                    /* as it was */
    lv_obj_set_size(wf, W, WF_H);
    lv_obj_set_pos(wf, 0, H - WF_H);
    waterfall_set_height(WF_H);
    waterfall_redraw();
    loop(4 * STEP);
}

int main(void) {
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t         px[W * H];     /* as gui_main.c's, the whole screen: the image drawn in one piece */
    static lv_disp_drv_t      drv;

    gui_thread = pthread_self();
    sony_16 = *lv_font_default();   /* the 3D labels are set in it */
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

    /* The waterfall's frame as gui_styles.c gives it: none, its children placed from its edge (LVGL's default theme
     * gave it a padding and a border, which the screen's shade checks would see) */
    lv_style_init(&waterfall_style);
    lv_style_set_border_width(&waterfall_style, 0);
    lv_style_set_radius(&waterfall_style, 0);
    lv_style_set_pad_all(&waterfall_style, 0);

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

    printf("A row the DSP thread writes after the refresh counted the new ones, before it paints them\n");
    row();
    int n = __atomic_exchange_n(&new_rows, 0, __ATOMIC_ACQUIRE);    /* refresh_waterfall()'s count: 1 */

    row();
    image_redraw(n);                                                /* its paint */
    picture_is_rows("that refresh");
    loop(100);
    picture_is_rows("the next one, for the second row");

    printf("The same in a slide\n");
    tune(-500);
    loop(STEP);                     /* the refresh for the tune, and the image drawn: the slide's first step */
    for (int i = 0; i < 20 && LV_ABS(wf_center_freq - wfp_center) * WIDTH < (int32_t)shown_span(); i++) {
        do_scroll_timer(NULL);      /* its next ones at the next draws, till the next refresh moves the image */
    }
    njobs = 0;
    n = __atomic_exchange_n(&new_rows, 0, __ATOMIC_ACQUIRE);
    row();

    int32_t was = wfp_center;

    image_redraw(n);
    check(wfp_center != was, "that refresh slid the image (%d Hz)", wfp_center - was);
    picture_is_rows("that refresh");
    ms = slide_ms(1500);
    check(ms >= 0, "the slide ended (%d ms more)", ms);
    picture_is_rows("after the slide");

    printf("The filter shade on the screen, Passband or Outside (#250), the VFO line hidden\n");
    params.waterfall_center_line.x = false;
    lv_obj_add_flag(vfo_line, LV_OBJ_FLAG_HIDDEN);
    shade_is("Passband, Full (as up to v0.3.11)");
    where = FILTER_SHADE_OUTSIDE;
    on_filter_color_change(NULL, NULL);
    shade_is("Outside, Full");
    shade = 1;
    on_filter_color_change(NULL, NULL);
    shade_is("Outside, 25 %");
    shade = 0;
    on_filter_color_change(NULL, NULL);
    shade_is("Outside, Edges");
    where = FILTER_SHADE_PASSBAND;
    on_filter_color_change(NULL, NULL);
    shade_is("Passband, Edges");
    where = FILTER_SHADE_OUTSIDE;
    shade = 4;
    on_filter_color_change(NULL, NULL);
    shade_is("Outside, Full again");
    filter_high = 1200;
    on_filter_change(NULL, NULL);
    shade_is("Outside, a 1.2 kHz filter: its left edge moved");
    loop(2 * STEP);
    filter_high = 1300;             /* LSB, 100 Hz at 125 Hz a pixel: the left edge a column on */
    on_filter_change(NULL, NULL);

    int32_t waiting = columns_waiting();

    check(waiting > 0 && waiting < 40, "a 1 px move of the edge: %d columns drawn again, not the shade's %d", waiting,
          WIDTH - (filter_band->coords.x2 - filter_band->coords.x1 + 1));
    shade_is("Outside, after it");
    cur_mode = radio_mode_usb;
    on_filter_change(NULL, NULL);
    shade_is("Outside, USB: both edges moved, over the line");
    params.waterfall_zoom.x = true;
    cur_zoom = 32;
    filter_high = 2700;
    on_zoom_changed((Subject *)cfg_mode_zoom, NULL);
    shade_is("Outside, zoom 32: the passband partly off the right edge");
    cur_zoom = 1;
    cur_mode = radio_mode_lsb;
    on_zoom_changed((Subject *)cfg_mode_zoom, NULL);
    shade_is("Outside, zoom 1 and LSB again");
    params.waterfall_zoom.x = false;
    where = FILTER_SHADE_PASSBAND;
    on_filter_color_change(NULL, NULL);
    shade_is("Passband again");
    params.waterfall_center_line.x = true;
    lv_obj_clear_flag(vfo_line, LV_OBJ_FLAG_HIDDEN);
    loop(2 * STEP);
    picture_is_rows("the picture under it as it was");

    printf("A row the DSP thread writes while a refresh paints the whole picture (1bit)\n");
    theme_shape.mono  = true;
    theme_shape.ink   = 0x000000;
    theme_shape.paper = 0xefece2;
    for (int v = 1; v < 256; v += 2) density[v] = 255;
    on_palette_change(NULL, NULL);          /* the waterfall again whole, in ink and paper */

    uint16_t top = newest;

    write_in = (long)WIDTH * (WF_H / 2);    /* half way down */
    loop(STEP);
    check(write_in == 0 && new_rows == 1, "the row came half way down the picture");
    picture_is(top, "that refresh, the rows as they were before it");
    loop(100);
    picture_is_rows("the next one, for that row");

    printf("Where a row has the carrier in CW and CW-R, a 700 Hz key tone, by the X6100's base (#191)\n");
    skip_rows = 0;              /* at rest: no rows of a frequency before to drop */
    retuned = false;

    const int32_t bin = SPAN_HZ / WATERFALL_NFFT;
    const uint32_t v118 = HW_CAP_TX_IQ_OFFSET;
    const uint32_t v119 = HW_CAP_TX_IQ_OFFSET | HW_CAP_ZOOM | HW_CAP_IF_SHIFT | HW_CAP_TX_SPECTRUM_ON_DIAL;
    const struct {
        const char *mode;
        int32_t     mode_id;
    } cws[] = {{"CW", radio_mode_cw}, {"CW-R", radio_mode_cwr}};

    /* Measured on the bench X6100, 1.1.8: the carrier on the line in CW and CW-R */
    for (size_t i = 0; i < 2; i++) {
        int32_t rx = lo_offset_rx(cws[i].mode_id, 700, v118), at;

        lo_offsets(rx, lo_offset_tx(cws[i].mode_id, 700, v118, 1));
        x6100_frame(false, rx, 0);
        at = carrier_at();
        check(LV_ABS(at) < bin, "Xiegu's 1.1.8, %s, receive: a station on the dial on the line (%+d Hz)", cws[i].mode,
              at);
        x6100_frame(true, rx, 0);
        at = carrier_at();
        check(LV_ABS(at) < bin, "Xiegu's 1.1.8, %s, TX: the carrier on the line (%+d Hz)", cws[i].mode, at);
    }

    printf("Xiegu's 1.1.9 at zoom 8, as the reporter's: its TX frames as if unzoomed\n");
    const int32_t z = 8, zbin = SPAN_HZ / z / WATERFALL_NFFT;
    const int32_t tones[] = {500, 700, 1200};

    for (size_t i = 0; i < 2; i++) {
        for (size_t t = 0; t < sizeof(tones) / sizeof(tones[0]); t++) {
            int32_t rx = lo_offset_rx(cws[i].mode_id, tones[t], v119), at;

            lo_offsets(rx, lo_offset_tx(cws[i].mode_id, tones[t], v119, z));
            x6100_frame_z(false, rx, 0, z);
            at = carrier_at();
            check(LV_ABS(at) < zbin, "%s, %d Hz, receive: a station on the dial on the line (%+d Hz)", cws[i].mode,
                  tones[t], at);
            x6100_119_tx_frame(rx, z);
            at = carrier_at();
            check(LV_ABS(at) < zbin, "%s, %d Hz, TX: the carrier on the line (%+d Hz)", cws[i].mode, tones[t], at);
            /* As v0.3.11 to v0.3.13 placed it: on the dial, the carrier the key tone over the zoom off the line */
            lo_offsets(rx, 0);
            x6100_119_tx_frame(rx, z);
            at = carrier_at();
            check(LV_ABS(at + rx / z) < zbin, "%s, %d Hz, TX placed on the dial: %+d Hz off the line (%+d Hz)",
                  cws[i].mode, tones[t], -rx / z, at);
        }
    }

    printf("The same base at Speed 1, three frames a line: a line of TX or of receive, not of both\n");
    int32_t lo_rx_cw = lo_offset_rx(radio_mode_cw, 700, v119);

    lo_offsets(lo_rx_cw, lo_offset_tx(radio_mode_cw, 700, v119, z));
    speed = 1;
    x6100_frame_z(false, lo_rx_cw, 0, z);
    x6100_frame_z(false, lo_rx_cw, 0, z);
    uint16_t was_newest = newest;

    x6100_119_tx_frame(lo_rx_cw, z);
    check(newest != was_newest && noise_level() == 0 && LV_ABS(carrier_at()) < zbin,
          "the first TX frame: a line of it alone, the carrier on the line (%+d Hz, beside it %d)", carrier_at(),
          noise_level());
    x6100_119_tx_frame(lo_rx_cw, z);
    x6100_119_tx_frame(lo_rx_cw, z);
    was_newest = newest;
    x6100_frame_z(false, lo_rx_cw, 0, z);
    check(newest != was_newest && noise_level() == 10 && LV_ABS(carrier_at()) < zbin,
          "the first receive frame after TX: a line of it alone, the station on the line (%+d Hz, beside it %d)",
          carrier_at(), noise_level());
    speed = 3;

    printf("Xiegu's 1.1.9 at zoom 8 with an IF shift: tuned that far from the dial, its receive frames centred there, "
           "its TX ones not\n");
    const int32_t shifts[] = {1500, -3000};     /* -3000: the reporter's, 21 Nov */
    const struct {
        const char *mode;
        int32_t     mode_id, tone;
    } shifted[] = {
        {"USB", radio_mode_usb, 700},
        {"CW", radio_mode_cw, 700},
        {"CW-R", radio_mode_cwr, 700},
        {"CW", radio_mode_cw, 1200},
        {"CW-R", radio_mode_cwr, 1200},
    };

    for (size_t s = 0; s < sizeof(shifts) / sizeof(shifts[0]); s++) {
        if_shift_to(shifts[s]);
        for (size_t i = 0; i < sizeof(shifted) / sizeof(shifted[0]); i++) {
            int32_t rx = lo_offset_rx(shifted[i].mode_id, shifted[i].tone, v119), at;

            lo_offsets(rx, lo_offset_tx(shifted[i].mode_id, shifted[i].tone, v119, z));
            x6100_frame_z(false, rx - shifts[s], 0, z);
            at = carrier_at();
            check(LV_ABS(at) < zbin, "%+d Hz, %s, %d Hz, receive: a station on the dial on the line (%+d Hz)",
                  shifts[s], shifted[i].mode, shifted[i].tone, at);
            x6100_119_tx_frame(rx, z);
            at = carrier_at();
            check(LV_ABS(at) < zbin, "%+d Hz, %s, %d Hz, TX: the carrier on the line, as the spectrum has it (%+d Hz)",
                  shifts[s], shifted[i].mode, shifted[i].tone, at);
        }
    }

    printf("The same at Speed 1 in USB: a line of TX or of receive, not of both, there the IF shift apart\n");
    const int32_t shift = 1500;

    if_shift_to(shift);
    lo_offsets(0, 0);
    speed = 1;
    x6100_frame_z(false, -shift, 0, z);
    x6100_frame_z(false, -shift, 0, z);
    was_newest = newest;
    x6100_119_tx_frame(0, z);
    check(newest != was_newest && noise_level() == 0 && LV_ABS(carrier_at()) < zbin,
          "the first TX frame: a line of it alone, the carrier on the line (%+d Hz, beside it %d)", carrier_at(),
          noise_level());
    x6100_119_tx_frame(0, z);
    x6100_119_tx_frame(0, z);
    was_newest = newest;
    x6100_frame_z(false, -shift, 0, z);
    check(newest != was_newest && noise_level() == 10 && LV_ABS(carrier_at()) < zbin,
          "the first receive frame after TX: a line of it alone, the station on the line (%+d Hz, beside it %d)",
          carrier_at(), noise_level());
    speed = 3;
    if_shift_to(0);

    printf("A patched base, which sends where it is tuned: its rows as before, with an IF shift too\n");
    lo_offsets(-700, -700);
    x6100_frame(false, -700, fg_freq - 700);
    check(rows[newest].center_freq == (uint32_t)fg_freq - 700, "receive: the row centred on the frequency it sent");
    x6100_frame(true, -700, fg_freq);
    check(rows[newest].center_freq == (uint32_t)fg_freq - 700, "TX: the row centred the key tone below the dial it sent");
    if_shift_to(shift);
    x6100_frame(false, -700 - shift, fg_freq - 700 - shift);
    check(rows[newest].center_freq == (uint32_t)(fg_freq - 700 - shift),
          "receive, the IF shift on: the row centred on the frequency it sent");
    x6100_frame(true, -700, fg_freq);
    check(rows[newest].center_freq == (uint32_t)fg_freq - 700,
          "TX, the IF shift on: the row centred the key tone below the dial it sent");
    if_shift_to(0);

    printf("Another height keeps the rows that fit (#302): %d lines to %d, a row more, then to %d\n", WF_H, TALL, LOW);
    static uint8_t had[TALL];
    int            off = 0;

    theme_shape.mono = false;
    lo_offsets(0, 0);
    lv_obj_set_y(obj, 0);
    for (int i = 0; i < WF_H; i++) row();
    loop(300);
    for (int y = 0; y < WF_H; y++) had[y] = row_at(y)->values[DATA_AT];
    waterfall_set_height(TALL);
    loop(300);
    for (int y = 0; y < TALL; y++) {
        off += y < WF_H ? row_at(y)->width == 0 || row_at(y)->values[DATA_AT] != had[y] : row_at(y)->width != 0;
    }
    check(height == TALL && !off, "taller: the %d rows it had, newest first, the rest empty", WF_H);
    check(history_is(had, WF_H), "the picture: those lines at the top, nothing under them");
    row();
    loop(100);
    memmove(had + 1, had, WF_H);
    had[0] = level;
    check(history_is(had, WF_H + 1), "a new row: at the top, the others a line down");
    waterfall_set_height(LOW);
    loop(300);
    off = 0;
    for (int y = 0; y < LOW; y++) off += row_at(y)->width == 0 || row_at(y)->values[DATA_AT] != had[y];
    check(height == LOW && !off, "lower: its newest %d rows", LOW);
    check(history_is(had, LOW), "the picture: those lines");
    row();
    loop(100);
    memmove(had + 1, had, LOW - 1);
    had[0] = level;
    check(history_is(had, LOW), "a new row: at the top, the others a line down");

    printf("The walkie screen's mini waterfall: %d x %d of the same rows, over the same 100 kHz\n", MW, MH);
    int at, peak, edge, wrong = 0;

    theme_shape.mono = false;
    lo_offsets(0, 0);
    for (int i = 0; i < MH + 5; i++) x6100_frame(false, 0, fg_freq);
    check(waterfall_mini_draw(mini_px, MW, MH, fg_freq, SPAN_HZ), "the first call draws it");
    for (int y = 0; y < MH; y++) {
        at = mini_peak(y, &peak, &edge);
        wrong += LV_ABS(at - MW / 2) > 1 || peak < 40 || edge != 10;
    }
    at = mini_peak(MH - 1, &peak, &edge);
    check(!wrong, "a station on the dial: a line down the middle of every row (the last: x %d, level %d, %d beside)",
          at, peak, edge);
    check(!waterfall_mini_draw(mini_px, MW, MH, fg_freq, SPAN_HZ), "no new row: nothing drawn");
    row();
    check(waterfall_mini_draw(mini_px, MW, MH, fg_freq, SPAN_HZ), "a new row: drawn");
    mini_peak(0, &peak, &edge);
    at = mini_peak(1, &wrong, &n);
    check(peak == level && edge == level && at == MW / 2 && n == 10,
          "it is the top line (level %d, the row's %d), the one before under it (its station at x %d)", peak, level,
          at);
    check(waterfall_mini_draw(mini_px, MW, MH, fg_freq + 10000, SPAN_HZ), "a view 10 kHz up: drawn again");
    at = mini_peak(MH - 1, &peak, &edge);
    check(LV_ABS(at - (MW / 2 - MW / 10)) <= 1 && peak >= 40, "the station a tenth of the width to the left (x %d)",
          at);
    lo_offsets(0, 0);
    d3_cases(wf);

    printf("%d of %d cases ok\n", good, cases);
    return good == cases ? 0 : 1;
}

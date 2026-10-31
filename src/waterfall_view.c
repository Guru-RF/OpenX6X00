/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 *
 *  The waterfall: the DSP's rows kept with the frequency each was taken at, drawn
 *  around the frequency shown now (it slides while tuning), with the VFO line and the
 *  passband over it. With the DRM planes it is drawn on the primary plane under the GUI,
 *  else into an LVGL image.
 */
#include "waterfall_view.h"

#include "band_names.h"
#include "cfg/cfg_api.h"
#include "dsp.h"
#include "smeter.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "gui_styles.h"
#include "theme.h"
#include "mono.h"
#include "util.h"
#include "lv_drivers/display/drm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH       800
#define MAX_HEIGHT  480     /* the buffers: the height changes live (spectrum on/off, its height) */
#define SPAN_HZ     100000  /* shown at zoom 1 */
#define DATA_AT     (WATERFALL_NFFT / 2)    /* a row's data from this value on, as many again around them */
#define ROW_BINS    (WATERFALL_NFFT * 2)
#define TX_MIN      S4      /* the colour scale in TX, and the automatic one before the DSP sets it */
#define TX_MAX      S9_20

/* One row as the DSP gave it: colour indexes over the span it covered, and in Fixed mode
 * half that span each side of it with the last picture received there (keep_around()) */
typedef struct {
    uint8_t  values[ROW_BINS];  /* the data from DATA_AT on; values[0] is width below the centre */
    uint32_t center_freq;
    uint32_t width;     /* Hz of the data; 0: empty */
    uint8_t  floor;     /* 75th percentile: above it the row has signals */
    uint8_t  seq;       /* 1bit: the dither row it keeps while it scrolls down */
    bool     kept;      /* the values around the data are set */
} wf_row_t;

static lv_obj_t     *obj;
static lv_obj_t     *img;               /* no planes: the waterfall as an image */
static lv_img_dsc_t *frame;
static bool          ready;             /* the buffers are made */

static lv_style_t    vfo_line_style;
static lv_obj_t     *vfo_line;
static lv_obj_t     *filter_band;
static lv_point_t    vfo_line_points[] = {{0, 0}, {0, 0}};

static lv_coord_t    height;
static wf_row_t     *rows;              /* a ring of height + 1 rows: those drawn, and the one being written */
static uint16_t      newest;            /* the last row written: the DSP thread writes the next, then moves this on */

static float         level_min = TX_MIN;
static float         level_max = TX_MAX;

static uint8_t       skip_rows;         /* rows of the old frequency still coming after a retune */
static bool          retuned;           /* since the row before (#51) */
static int32_t       radio_center_freq; /* where the base is tuned */
static int32_t       wf_center_freq;    /* the frequency at the centre of the picture */
static int32_t       mode_lo_offset;
static int32_t       if_shift;
static uint8_t       zoom = 1;
static uint8_t       line_zoom = 1;  /* the zoom the VFO line and the passband were placed at */

static uint8_t       refresh_period = 1;   /* draw one refresh in so many (dialogs slow it down) */
static uint8_t       refresh_count;

/* Direct mode (DRM planes): the waterfall is drawn on the primary plane under
 * the GUI, which is transparent there. wfp is its portrait image: landscape
 * row y of the waterfall is portrait column y, landscape x is row 799 - x.
 * A new row shifts the image by one column and draws only that column; a new
 * centre or zoom redraws it all */
static bool             direct = false;
static uint32_t         *wfp;
static int32_t          wfp_head;   /* each portrait row of wfp is a ring: the newest column is at wfp_head */
static int32_t          wfp_center = 0;
static uint32_t         wfp_bw = 0;
static bool             wfp_full = true;
static int              new_rows = 0;       /* rows from the DSP thread not drawn yet */

/* Fixed mode: RF frequency at the display centre, valid while view_fixed */
static int32_t          view_center = 0;
static bool             view_fixed = false;

/* Fixed mode keeps the picture still while the VFO moves across it, and the base sends only
 * the span around the VFO: the side tuned away from gets no new data (#10). What was last
 * received at each frequency around the rows (dB, NAN: nothing) fills it there instead of the
 * noise. keep_db[i] is keep_bin + i - DATA_AT bins (of keep_width / WATERFALL_NFFT Hz) above
 * keep_anchor, at a row's values[i] but for the part of a bin the row is off this grid: it moves
 * by whole bins only, where the rows move by the Hz of each tuning step; moved by the parts of a
 * bin, the picture would drift or blur. The DSP thread's */
static float            keep_db[ROW_BINS];
static uint32_t         keep_anchor;
static int32_t          keep_bin;
static uint32_t         keep_width;         /* 0: nothing kept */

static void refresh_waterfall(void *arg);
static void do_scroll_timer(lv_timer_t *t);
static void update_middle_line(void);
static void make_objs(void);

/* Settings (web Display page): the theme's colours or one of these */
static void on_palette_change(Subject *subj, void *user_data) {
    extern const uint32_t wf_palette_legacy[256], wf_palette_gauss[256];   /* styles.c */

    if (theme_shape.mono) {     /* 1bit: its ink density, not the presets */
        wf_palette = theme_waterfall();
        return;
    }
    static const uint32_t grey[]    = {0x000000, 0xffffff};
    static const uint32_t fire[]    = {0x000000, 0x400000, 0xa00000, 0xff4000, 0xffa000, 0xffff60, 0xffffff};
    static const uint32_t ice[]     = {0x000000, 0x001040, 0x0040a0, 0x0090ff, 0x40e0ff, 0xffffff};
    static const uint32_t rainbow[] = {0x000000, 0x0000a0, 0x0080ff, 0x00e0a0, 0x80ff00, 0xffe000, 0xff4000, 0xff0000};
    static uint32_t       preset[256];

    switch (param_i_get(cfg_wf_palette)) {
        case 1: wf_palette = wf_palette_legacy; return;
        case 2: wf_palette = wf_palette_gauss; return;
        case 3: theme_ramp_palette(grey, 2, 0, preset); break;
        case 4: theme_ramp_palette(fire, 7, 0, preset); break;
        case 5: theme_ramp_palette(ice, 6, 0, preset); break;
        case 6: theme_ramp_palette(rainbow, 8, 0, preset); break;
        default: wf_palette = theme_waterfall(); return;
    }
    wf_palette = preset;
}

/* The row age rows before the newest (0: the newest), age under height: never the one the DSP thread
 * is writing */
static wf_row_t *row_at(int32_t age) {
    int32_t ring = height + 1;

    return &rows[(__atomic_load_n(&newest, __ATOMIC_ACQUIRE) - age + ring) % ring];
}

/* ---- Rows from the DSP thread ---- */

/* A part out of the row's band (tuned away from it) shows the row's own noise, its
 * values at scattered places capped where signals start, not black */
static inline uint8_t noise_at(const wf_row_t *row, size_t i) {
    uint8_t v = row->values[DATA_AT + (i * 2654435761u + (uintptr_t) row) % WATERFALL_NFFT];

    return v < row->floor ? v : row->floor;
}

/* Where hz is in keep_db, bins from keep_anchor */
static double keep_pos(uint32_t hz) {
    return ((double) hz - keep_anchor) * WATERFALL_NFFT / keep_width;
}

/* Fixed mode: a new row's data (db, its colours from min to max) into keep_db (learn: not a row
 * that may still be of the frequency before), and the row's values around its data from keep_db */
static void keep_around(wf_row_t *row, const float *db, float min, float max, bool learn) {
    if (row->width != keep_width || fabs(keep_pos(row->center_freq) - DATA_AT - keep_bin) >= ROW_BINS) {
        /* The first, another zoom or another band: nothing kept */
        keep_width = row->width;
        keep_anchor = row->center_freq;
        keep_bin = -DATA_AT;
        for (size_t i = 0; i < ROW_BINS; i++) {
            keep_db[i] = NAN;
        }
    }
    /* keep_db follows the row by whole bins: the row's value j is at keep_db[j + frac] */
    double  at = keep_pos(row->center_freq) - DATA_AT;
    int32_t move = lround(at) - keep_bin;
    int32_t gone = LV_MIN(LV_ABS(move), ROW_BINS);
    float   frac = at - lround(at);

    if (move > 0) {
        memmove(keep_db, keep_db + gone, (ROW_BINS - gone) * sizeof(float));
        for (int32_t i = ROW_BINS - gone; i < ROW_BINS; i++) keep_db[i] = NAN;
    } else if (move < 0) {
        memmove(keep_db + gone, keep_db, (ROW_BINS - gone) * sizeof(float));
        for (int32_t i = 0; i < gone; i++) keep_db[i] = NAN;
    }
    keep_bin += move;

    /* The data, received now */
    for (int32_t k = 0; learn && k < ROW_BINS; k++) {
        float   x = k - frac - DATA_AT;
        int32_t i = floorf(x);

        if (i >= 0 && i < WATERFALL_NFFT - 1) {
            keep_db[k] = db[i] + (db[i + 1] - db[i]) * (x - i);
        } else if (i == WATERFALL_NFFT - 1 && x == i) {
            keep_db[k] = db[i];
        }
    }

    /* Around it what was received there last, the row's noise where nothing was */
    float scale = 255.0f / (max - min);

    for (int32_t j = 0; j < ROW_BINS; j++) {
        if (j >= DATA_AT && j < DATA_AT + WATERFALL_NFFT) {
            continue;
        }
        float   x = j + frac;
        int32_t i = floorf(x);
        float   a = i >= 0 ? keep_db[i] : NAN;
        float   b = i + 1 < ROW_BINS ? keep_db[i + 1] : NAN;
        float   v = isnan(a) ? b : isnan(b) ? a : a + (b - a) * (x - i);

        if (isnan(v)) {
            row->values[j] = noise_at(row, j);
        } else {
            v = (v - min) * scale;
            row->values[j] = v < 0.0f ? 0 : v > 255.0f ? 255 : v;
        }
    }
    row->kept = true;
}

static uint8_t row_seq;     /* 1bit: the next row's dither row */

void waterfall_data(float *db, uint16_t size, bool tx, uint32_t base_freq, uint32_t width_hz) {
    /* After a retune the base's next rows may still be of the frequency before: from rest, the two
     * are dropped. With the dial turning on, every row was, and the waterfall stood still until it
     * rested (#51): a retune within five rows of the one before is the dial turning, and then the
     * rows come as they are, at most a step behind. A row that may be behind is drawn, not kept
     * (Fixed mode) */
    static uint8_t still = UINT8_MAX;   /* rows since the last retune */
    static bool    turning;
    bool           fresh = true;

    if (retuned && base_freq == 0) {
        retuned = false;
        turning = still < 5;
        still = 0;
    } else if (still < UINT8_MAX) {
        still++;
    }
    if (skip_rows && base_freq == 0) {
        if (!turning) {
            skip_rows--;
            return;
        }
        skip_rows = 0;
        fresh = false;
    }

    /* Speed 1 and 2 average 3 and 2 frames into one line (5 and 7.5 lines/s);
     * 4 and 5 get faster frames from the DSP */
    static const uint8_t speed_frames[] = {3, 2, 1, 1, 1};
    static float    acc[WATERFALL_NFFT];
    static int      acc_n, acc_frames;     /* the frames in the line, and since the line before */
    static bool     acc_fresh;
    static uint32_t acc_freq;
    int             frames = speed_frames[LV_CLAMP(1, param_i_get(cfg_waterfall_speed), 5) - 1];
    /* Where the frame was taken: with no frequency from the base (the OEM X6100 base) the GUI's,
     * else a line averaged frames of several tuning steps */
    uint32_t        at = base_freq ? base_freq : radio_center_freq + mode_lo_offset;

    if (frames > 1 && size <= WATERFALL_NFFT) {
        if (acc_n == 0 || at != acc_freq) {
            /* first frame, or the radio moved: the line again from this frame (one frequency in it) */
            memcpy(acc, db, size * sizeof(float));
            acc_n = 1;
            acc_freq = at;
            acc_fresh = fresh;
        } else {
            liquid_vectorf_add(acc, db, size, acc);
            acc_n++;
            acc_fresh &= fresh;
        }
        /* a line every `frames` frames, the dial turning or not: restarted at each step, it came only
         * once the dial rested (#51) */
        if (++acc_frames < frames) {
            return;
        }
        liquid_vectorf_mulscalar(acc, size, 1.0f / acc_n, acc);
        db = acc;
        fresh = acc_fresh;
        acc_n = acc_frames = 0;
    }

    /* The row is made in the slot after the newest, which is never drawn, and shown once whole.
     * Moved on first, the GUI could draw it half made, a column off, with the sides of the row the
     * slot held around its data */
    uint16_t next = (newest + 1) % (height + 1);
    float    min, max;

    waterfall_get_levels(tx, &min, &max);
    if (base_freq == 0) {
        base_freq = radio_center_freq + mode_lo_offset;     /* no frequency with the data: the one set */
    } else if (tx) {
        base_freq += mode_lo_offset;
    }

    wf_row_t *row = &rows[next];
    float     scaled[size];
    uint16_t  hist[256] = {0};

    row->center_freq = base_freq;
    row->width = width_hz;

    /* dB to the 256 colours of the scale */
    liquid_vectorf_addscalar(db, size, -min, scaled);
    liquid_vectorf_mulscalar(scaled, size, 255.0f / (max - min), scaled);
    for (uint16_t i = 0; i < size; i++) {
        uint8_t c = scaled[i] < 0.0f ? 0 : scaled[i] > 255.0f ? 255 : scaled[i];

        row->values[DATA_AT + i] = c;
        hist[c]++;
    }
    uint32_t n = 0, id = 0;
    while (id < 255 && (n += hist[id]) < size * 3 / 4) id++;
    row->floor = id;
    row->seq = row_seq++;

    /* Not the picture of TX: after it the same frequencies are received again */
    row->kept = false;
    if (!param_i_get(cfg_wf_mode)) {
        keep_width = 0;
    } else if (!tx && size == WATERFALL_NFFT && width_hz) {
        keep_around(row, db, min, max, fresh);
    }
    __atomic_store_n(&newest, next, __ATOMIC_RELEASE);
    __atomic_add_fetch(&new_rows, 1, __ATOMIC_RELEASE);
    scheduler_put_coalesced(refresh_waterfall);
}

/* ---- One row onto the screen's pixels ---- */

/* Pixels i0..i1-1 (out is indexed by pixel) of a row, for a picture of span Hz centred on
 * center: linear between the row's points, 1/8 of a point steps. Its values: its data, and in
 * Fixed mode around them the last picture; out of them its noise */
static void row_pixels(const wf_row_t *row, uint32_t center, uint32_t span, size_t i0, size_t i1, uint8_t *out) {
    if (!row->width) {
        memset(out + i0, 0, i1 - i0);       /* not written yet */
        return;
    }
    uint32_t first = row->kept ? 0 : DATA_AT;
    uint32_t end = row->kept ? ROW_BINS : DATA_AT + WATERFALL_NFFT;
    uint32_t reach = row->kept ? row->width : row->width / 2;
    int32_t  grid_lo = row->center_freq - row->width;  /* values[0] */
    uint32_t row_lo = row->center_freq - reach;
    uint32_t row_hi = row->center_freq + reach;
    uint32_t lo = center - span / 2;
    uint32_t hi = lo + span;
    uint32_t half = span / 2;
    bool     apart = row_lo > hi || row_hi < lo;

    for (size_t i = i0; i < i1; i++) {
        int32_t  hz = center + (span * i + half) / WIDTH - half;
        int32_t  d = hz - grid_lo;
        uint32_t pos = d < 0 ? 0 : (uint32_t) d * (WATERFALL_NFFT << 3) / row->width;
        uint32_t k = pos >> 3;

        if (apart || d < 0 || k < first || k + 2 >= end) {
            out[i] = noise_at(row, i);
        } else {
            int16_t a = row->values[k], b = row->values[k + 1];

            out[i] = a + (((b - a) * (pos - (k << 3))) >> 3);
        }
    }
}

/* The zoom the picture is drawn at: the spectrum's, or 1 with the zoom switch of Settings "Line, zoom" off */
static uint8_t shown_zoom(void) {
    return params.waterfall_zoom.x ? zoom : 1;
}

/* The span the picture shows now */
static uint32_t shown_span(void) {
    return SPAN_HZ / shown_zoom();
}

/* ---- Direct mode ---- */

/* 1bit: the column of the dither pattern landscape pixel 0 is at; it moves with the slides, so a
 * pixel keeps the dots it was drawn with */
static uint8_t dither_x;

/* A level's pixel: its palette colour, or in 1bit ink or paper by its ink density, dithered where
 * the pattern stays with the picture (the row's own dither row, the column it moves with): fixed
 * to the screen, the dots would crawl as the rows scroll */
static inline uint32_t wf_pixel(uint8_t v, int x, const wf_row_t *row) {
    if (!theme_shape.mono) return wf_palette[v];
    return (mono_dither(theme_wf_density()[v], x, row->seq) ? theme_shape.ink : theme_shape.paper) | 0xff000000;
}

/* Pixels i0..i1-1 of a landscape row (age 0 the newest) into portrait column age
 * of wfp, at the image's centre */
static void direct_column(int32_t age, uint32_t span, size_t i0, size_t i1) {
    uint8_t         px[WIDTH];
    const wf_row_t *row = row_at(age);

    row_pixels(row, wfp_center, span, i0, i1, px);
    for (size_t i = i0; i < i1; i++) {
        wfp[(WIDTH - 1 - i) * height + (wfp_head + age) % height] = wf_pixel(px[i], i + dither_x, row) | 0xff000000;
    }
}

/* Direct mode: the waterfall area of the GUI fully transparent (alpha 0), drawn
 * after the parents and before the children (VFO line, grid, spots), so the
 * primary plane shows through whatever is behind in the GUI */
void direct_hole_cb(lv_event_t *e) {
    lv_draw_ctx_t   *ctx = lv_event_get_draw_ctx(e);
    lv_area_t       a;
    int32_t         stride = lv_area_get_width(ctx->buf_area);

    if (!_lv_area_intersect(&a, &lv_event_get_target(e)->coords, ctx->clip_area)) {
        return;
    }
    for (int32_t y = a.y1; y <= a.y2; y++) {
        lv_color_t *row = (lv_color_t *) ctx->buf + (y - ctx->buf_area->y1) * stride + (a.x1 - ctx->buf_area->x1);

        memset(row, 0, lv_area_get_width(&a) * sizeof(lv_color_t));
    }
}

static bool shown(lv_obj_t *o) {
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}

static void direct_redraw(int new_row) {
    uint32_t    bandwidth = shown_span();
    lv_area_t   a;

    if (!shown(obj)) {
        wfp_full = true;            /* not up to date when shown again */
        return;
    }
    /* Tuning moves the centre: the image slides by whole pixels and only the
     * columns coming in are computed (it was all of it at each VFO step, which
     * kept the GUI thread busy while tuning). Moves under a pixel wait. Rows
     * that piled up while the thread was busy scroll in the same way: a full
     * redraw for them made it slower still, and more rows piled up. */
    bool    full = wfp_full || new_row >= height || wfp_bw != bandwidth;
    int32_t px   = 0;

    if (!full && wfp_center != wf_center_freq) {
        px   = ((int64_t) wf_center_freq - wfp_center) * WIDTH / (int64_t) bandwidth;
        full = LV_ABS(px) >= WIDTH / 2;
    }
    if (full) {
        wfp_center = wf_center_freq;
        wfp_bw = bandwidth;
        for (int32_t col = 0; col < height; col++) {
            direct_column(col, bandwidth, 0, WIDTH);
        }
        wfp_full = false;
    } else if (px || new_row) {
        /* The new rows first, at the old centre; then the slide moves them with the rest */
        if (new_row) {
            /* The ring turns: nothing moves (a memmove of the whole image per row before) */
            wfp_head = (wfp_head - new_row + height) % height;
            for (int32_t col = 0; col < new_row; col++) {
                direct_column(col, bandwidth, 0, WIDTH);
            }
        }
        if (px) {
            size_t i0, i1;

            /* Landscape pixel i now shows the old i + px: portrait row R the old R - px */
            wfp_center += (int64_t) px * bandwidth / WIDTH;
            dither_x += px;
            if (px > 0) {
                memmove(wfp + px * height, wfp, (WIDTH - px) * height * sizeof(uint32_t));
                i0 = WIDTH - px;
                i1 = WIDTH;
            } else {
                memmove(wfp, wfp - px * height, (WIDTH + px) * height * sizeof(uint32_t));
                i0 = 0;
                i1 = -px;
            }
            for (int32_t col = 0; col < height; col++) {
                direct_column(col, bandwidth, i0, i1);
            }
        }
    } else {
        return;
    }
    lv_obj_get_coords(obj, &a);
#if USE_DRM
    drm_wf_present_ring(wfp, a.y1, height, wfp_head);
#endif
}

/* ---- No planes: the whole image again at each refresh ---- */

static void image_redraw(void) {
    uint32_t    span = shown_span();
    lv_color_t *px = (lv_color_t *) frame->data;
    uint8_t     c[WIDTH];
    /* 1bit: the dither column by frequency, so the dots move with the picture while tuning */
    int         x0 = theme_shape.mono && span ? (int)((int64_t) wf_center_freq * WIDTH / span) : 0;

    for (int32_t y = 0; y < height; y++, px += WIDTH) {
        const wf_row_t *row = row_at(y);

        row_pixels(row, wf_center_freq, span, 0, WIDTH, c);
        for (size_t i = 0; i < WIDTH; i++) {
            px[i] = (lv_color_t) wf_pixel(c[i], x0 + i, row);
        }
    }
    lv_obj_invalidate(img);
}

static void refresh_waterfall(void *arg) {
    if (!ready || ++refresh_count < refresh_period) {
        return;
    }
    refresh_count = 0;
    if (shown_zoom() != line_zoom) {    /* "Line, zoom" switched: the line and the band to the new scale */
        update_middle_line();
    }
    if (direct) {
        direct_redraw(__atomic_exchange_n(&new_rows, 0, __ATOMIC_ACQUIRE));
    } else {
        image_redraw();
    }
}

void waterfall_refresh_reset() {
    refresh_period = 1;
}

void waterfall_refresh_period_set(uint8_t k) {
    if (k) {
        refresh_period = k;
    }
}

/* ---- Where the picture is centred ---- */

int32_t waterfall_view_shift(void) {
    if (!param_i_get(cfg_wf_mode)) {
        view_fixed = false;
        return 0;
    }
    int32_t rc = cparam_i_get(cfg_fg_freq) - param_i_get(cfg_band_if_shift);
    int32_t vfo = cparam_i_get(cfg_fg_freq);
    int32_t half = SPAN_HZ / 2 / param_i_get(cfg_mode_zoom);

    /* The received data (centred on rc) and the VFO within 40% of the span of the centre: the
     * line goes most of the way to the edge before the view jumps back, so what is on the screen
     * can be tuned without the picture moving, and the side tuned away from shows the last
     * picture received there (#10). v0.3.7 jumped at a quarter for rc, to show less of that, and
     * the reporter found it jumped too soon. The VFO's 40%: an IF shift keeps it up to that far
     * from rc (on_if_shift_zoom(), screen_main.c), and under it the view would follow it at
     * every step */
    if (!view_fixed || abs(rc - view_center) > half * 8 / 10 || abs(vfo - view_center) > half * 8 / 10) {
        view_center = rc;
        view_fixed = true;
    }
    return rc - view_center;
}

/* One step of the horizontal scroll towards the VFO (direct mode: every 20 ms) */
static void do_scroll_timer(lv_timer_t *t) {
    int32_t target = radio_center_freq - waterfall_view_shift();

    if (wf_center_freq == target) {
        return;
    }
    if (params.waterfall_smooth_scroll.x) {
        /* A critically damped spring, a step every 20 ms: it starts and stops
         * gently, never overshoots and is there in ~0.26 s (k and c from a
         * simulation; a tenth of the way per step took 2 s to settle) */
        static double pos, vel;
        const double  k = 0.2, c = 0.694;

        if (llround(pos) != wf_center_freq || LV_ABS(target - wf_center_freq) > SPAN_HZ) {
            pos = wf_center_freq;   /* moved elsewhere, or a band change: from here */
            vel = 0;
        }
        if (LV_ABS(target - wf_center_freq) > SPAN_HZ) {
            pos = target;           /* a band change: no slide across it */
        }
        vel += k * (target - pos) - c * vel;
        pos += vel;
        if (fabs(target - pos) < 1.0 && fabs(vel) < 1.0) {
            pos = target;
            vel = 0;
        }
        wf_center_freq = llround(pos);
    } else {
        wf_center_freq = target;
    }
    scheduler_put_coalesced(refresh_waterfall);
}

/* No planes: a step each time the image is drawn */
static void image_drawn_cb(lv_event_t *e) {
    do_scroll_timer(NULL);
}

/* ---- The VFO line and the passband ---- */

static void update_middle_line(void) {
    /* At the picture's scale: with the zoom of "Line, zoom" off the waterfall shows 100 kHz at any spectrum zoom,
     * and the band came out that many times too wide, the line that far off the middle */
    line_zoom = shown_zoom();
    lv_coord_t width = theme_shape.mono ? 3 : line_zoom / 2 + 3;     /* 1bit: the paper halo of vfo_core_cb() */
    lv_coord_t vfo_x = (if_shift + waterfall_view_shift()) * line_zoom * WIDTH / SPAN_HZ;

    lv_obj_set_pos(vfo_line, vfo_x + width / 2, 0);
    lv_style_set_line_width(&vfo_line_style, width);

    /* The passband, same scale as the line: from the VFO up (USB), down
     * (LSB) or both ways (AM, FM), as on the spectrum */
    int32_t low = subject_i_get((SubjectInt*)cfg_cur_filter_low);
    int32_t high = subject_i_get((SubjectInt*)cfg_cur_filter_high);
    int32_t tone = param_i_get(cfg_key_tone);
    int32_t from, to;

    switch ((radio_mode_t) subject_i_get((SubjectInt*)cfg_cur_mode)) {
        case radio_mode_lsb:
        case radio_mode_lsb_dig:
            from = -high;
            to = -low;
            break;
        /* CW and CWR: the edges are tones, and the signal heard at the key tone is on the line, so
         * the band is around the line in both, as the spectrum draws it from its LO marker (#34: it
         * was beside it, a pitch off, above in CW and below in CWR). From the key tone, not from the
         * LO offset that places the rows: the X6200's offset is still to be checked on the radio */
        case radio_mode_cw:
            from = low - tone;
            to = high - tone;
            break;
        case radio_mode_cwr:
            from = tone - high;
            to = tone - low;
            break;
        case radio_mode_am:
        case radio_mode_nfm:
        case radio_mode_sam:
        case radio_mode_wfm:
            from = -high;
            to = high;
            break;
        default:
            from = low;
            to = high;
            break;
    }
    lv_coord_t x1 = WIDTH / 2 + vfo_x + from * line_zoom * WIDTH / SPAN_HZ;
    lv_coord_t x2 = WIDTH / 2 + vfo_x + to * line_zoom * WIDTH / SPAN_HZ;

    lv_obj_set_pos(filter_band, x1, 0);
    lv_obj_set_width(filter_band, LV_MAX(x2 - x1, 1));
}

/* Shown or not as set (params), checked at each draw */
static void vfo_line_cb(lv_event_t *e) {
    bool hide = !params.waterfall_center_line.x;

    if (hide != lv_obj_has_flag(vfo_line, LV_OBJ_FLAG_HIDDEN)) {
        if (hide) {
            lv_obj_add_flag(vfo_line, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(vfo_line, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 1bit: the VFO line 1 px of ink in its 3 px paper halo, readable over the dots and the carriers */
static void vfo_core_cb(lv_event_t *e) {
    lv_obj_t          *l = lv_event_get_target(e);
    lv_draw_line_dsc_t d;
    lv_point_t         a = {l->coords.x1, l->coords.y1}, b = {l->coords.x1, l->coords.y1 + height};

    lv_draw_line_dsc_init(&d);
    d.color = TC(vfo_line);
    d.width = 1;
    lv_draw_line(lv_event_get_draw_ctx(e), &d, &a, &b);
}

static void set_line_height(void) {
    vfo_line_points[1].y = height;
    lv_line_set_points(vfo_line, vfo_line_points, 2);
}

/* ---- Settings ---- */

static void on_zoom_changed(Subject *subj, void *user_data) {
    zoom = subject_i_get((SubjectInt *)subj);
    update_middle_line();
}

static void on_view_changed(Subject *subj, void *user_data) {
    update_middle_line();
    scheduler_put_coalesced(refresh_waterfall);
}

/* A new frequency: the next rows the base sends may still be of the old one */
static void on_if_shift_changed(Subject *subj, void *user_data) {
    skip_rows = 2;
    retuned = true;
    if_shift = subject_i_get((SubjectInt *)subj);
    radio_center_freq = cparam_i_get(cfg_fg_freq) - if_shift;
    update_middle_line();
}

static void on_fg_freq_change(Subject *subj, void *user_data) {
    skip_rows = 2;
    retuned = true;
    radio_center_freq = subject_i_get((SubjectInt *)subj) - if_shift;
}

static void on_mode_lo_offset_change(Subject *subj, void *user_data) {
    mode_lo_offset = subject_i_get((SubjectInt *)subj);
}

static void on_filter_change(Subject *subj, void *user_data) {
    update_middle_line();
}

static void on_filter_color_change(Subject *subj, void *user_data) {
    int32_t c = theme_filter_color(param_i_get(cfg_filter_color));

    lv_obj_set_style_bg_color(filter_band, c >= 0 ? lv_color_hex(c) : TC(vfo_line), 0);
}

/* Manual levels follow the band's settings; automatic ones come from the DSP */
static void on_levels_change(Subject *subj, void *user_data) {
    if (!param_i_get(cfg_auto_level_enabled)) {
        level_min = param_i_get(cfg_band_grid_min);
        level_max = param_i_get(cfg_band_grid_max);
    }
}

/* ---- Making it ---- */

lv_obj_t * waterfall_init(lv_obj_t * parent) {
    subject_subscribe_and_notify((Subject *)cfg_fg_freq, on_fg_freq_change, NULL);
    wf_center_freq = radio_center_freq;

    obj = lv_obj_create(parent);
    lv_obj_add_style(obj, &waterfall_style, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    lv_style_init(&vfo_line_style);
    lv_style_set_line_width(&vfo_line_style, 1);
    /* VFO line in amber so it stands out from the grey 20 kHz grid */
    lv_style_set_line_color(&vfo_line_style, TC(vfo_line));
    lv_style_set_line_opa(&vfo_line_style, LV_OPA_COVER);
    lv_style_set_pad_all(&vfo_line_style, 0);

    /* Filter passband: a light band of the VFO line colour, under the line (1bit: its two edges) */
    filter_band = lv_obj_create(obj);
    lv_obj_remove_style_all(filter_band);
    lv_obj_set_style_bg_opa(filter_band, theme_shape.mono ? LV_OPA_TRANSP : LV_OPA_20, 0);
    if (theme_shape.mono) {
        lv_obj_set_style_border_side(filter_band, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_width(filter_band, 1, 0);
        lv_obj_set_style_border_color(filter_band, TC(vfo_line), 0);
    }
    lv_obj_set_height(filter_band, LV_PCT(100));
    lv_obj_clear_flag(filter_band, LV_OBJ_FLAG_CLICKABLE);

    vfo_line = lv_line_create(obj);
    lv_obj_add_style(vfo_line, &vfo_line_style, 0);
    lv_obj_center(vfo_line);
    if (theme_shape.mono) {
        lv_obj_set_style_line_color(vfo_line, TC(panel), 0);
        lv_obj_add_event_cb(vfo_line, vfo_core_cb, LV_EVENT_DRAW_POST, NULL);
    }
    lv_obj_add_event_cb(obj, vfo_line_cb, LV_EVENT_DRAW_POST_END, NULL);

    subject_subscribe_delayed_and_notify((Subject *)cfg_mode_zoom, on_zoom_changed, NULL);
    subject_subscribe_delayed_and_notify((Subject *)cfg_band_if_shift, on_if_shift_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_cur_filter_low, on_filter_change, NULL);
    subject_subscribe_delayed((Subject *)cfg_cur_filter_high, on_filter_change, NULL);
    subject_subscribe_delayed((Subject *)cfg_cur_mode, on_filter_change, NULL);
    /* CW: a new key tone moves the band, the edges changed or not (the low one at 0 stays) */
    subject_subscribe_delayed((Subject *)cfg_key_tone, on_filter_change, NULL);
    subject_subscribe_delayed((Subject *)cfg_fg_freq, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_mode, on_view_changed, NULL);
    subject_subscribe_delayed_and_notify((Subject *)cfg_wf_palette, on_palette_change, NULL);
    subject_subscribe_delayed_and_notify((Subject *)cfg_filter_color, on_filter_color_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_mode_lo_offset, on_mode_lo_offset_change, NULL);
    subject_subscribe((Subject *)cfg_auto_level_enabled, on_levels_change, NULL);
    subject_subscribe((Subject *)cfg_band_grid_min, on_levels_change, NULL);
    subject_subscribe_and_notify((Subject *)cfg_band_grid_max, on_levels_change, NULL);

    if (ready) make_objs();
    return obj;
}

/* What goes on the buffers: made again on a theme change (main_screen_set_theme()), the buffers stay */
static void make_objs(void) {
    if (direct) {
        /* The GUI stays transparent here: the primary plane shows through */
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(obj, direct_hole_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    } else {
        img = lv_img_create(obj);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        lv_img_set_src(img, frame);
        lv_obj_add_event_cb(img, image_drawn_cb, LV_EVENT_DRAW_MAIN_END, NULL);
        lv_obj_move_background(img);
    }
    band_info_init(obj);
}

void waterfall_set_height(lv_coord_t h) {
    lv_obj_set_height(obj, h);
    lv_obj_update_layout(obj);
    height = lv_obj_get_height(obj);

    if (ready) {
        /* Again (main_screen.c): the same buffers, an empty waterfall of the new height. The
         * DSP thread may write a row meanwhile: it stays within the MAX_HEIGHT + 1 rows */
        __atomic_store_n(&newest, 0, __ATOMIC_RELEASE);
        for (size_t i = 0; i <= MAX_HEIGHT; i++) {
            rows[i].width = 0;
        }
        wfp_head = 0;
        wfp_full = true;
        if (!direct) {
            lv_img_buf_free(frame);
            frame = lv_img_buf_alloc(WIDTH, height, LV_IMG_CF_TRUE_COLOR);
            lv_img_set_src(img, frame);
        }
        set_line_height();
        scheduler_put_coalesced(refresh_waterfall);
        return;
    }

#if USE_DRM
    direct = drm_planes();
#endif
    if (direct) {
        wfp = calloc(WIDTH * MAX_HEIGHT, sizeof(uint32_t));
        lv_timer_create(do_scroll_timer, 20, NULL);
    } else {
        frame = lv_img_buf_alloc(WIDTH, height, LV_IMG_CF_TRUE_COLOR);
    }
    rows = calloc(MAX_HEIGHT + 1, sizeof(*rows));
    for (size_t i = 0; i <= MAX_HEIGHT; i++) {
        rows[i].center_freq = radio_center_freq;
    }
    newest = 0;
    waterfall_min_max_reset();
    make_objs();
    set_line_height();
    ready = true;
}

/* ---- Colour scale ---- */

void waterfall_min_max_reset() {
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

void waterfall_update_max(float db) {
    auto_level(&level_max, db);
}

void waterfall_update_min(float db) {
    auto_level(&level_min, db);
}

void waterfall_get_levels(bool tx, float *min, float *max) {
    if (tx) {
        *min = TX_MIN;
        *max = TX_MAX;
    } else {
        /* Reference level: shifts the whole colour scale, + is brighter */
        *min = level_min - param_i_get(cfg_waterfall_ref);
        *max = level_max - param_i_get(cfg_waterfall_ref);
        if (*max < *min + 10.0f) {
            *max = *min + 10.0f;
        }
    }
}

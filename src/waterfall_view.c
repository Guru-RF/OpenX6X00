/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 *
 *  The waterfall: the DSP's rows kept with the frequency each was taken at, drawn
 *  around the frequency shown now (it slides while tuning), with the VFO line and the
 *  passband over it. With the DRM planes it is drawn on the primary plane under the GUI;
 *  else into an LVGL image, scrolled the same way (new lines and tune slides only, not a
 *  full rebuild every refresh).
 */
#include "waterfall_view.h"

#include "board.h"
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
#include "freq_scale.h"
#include "screen_light.h"
#include "button_bar.h"
#include "walkie.h"
#include "lv_drivers/display/drm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH       BOARD_DISP_W
#define MAX_HEIGHT  BOARD_DISP_H     /* the buffers: the height changes live (spectrum on/off, its height) */
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
    uint32_t num;       /* its number, counted from the start: 3D keeps every third */
    uint32_t ms;        /* when it came (get_time()): 3D moves the traces between rows */
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

    wfp_full = true;            /* colours change: the image / plane must be remade */
    if (theme_shape.mono) {     /* 1bit: its ink density, not the presets */
        wf_palette = theme_waterfall();
    } else {
        static const uint32_t grey[]    = {0x000000, 0xffffff};
        static const uint32_t fire[]    = {0x000000, 0x400000, 0xa00000, 0xff4000, 0xffa000, 0xffff60, 0xffffff};
        static const uint32_t ice[]     = {0x000000, 0x001040, 0x0040a0, 0x0090ff, 0x40e0ff, 0xffffff};
        static const uint32_t rainbow[] = {0x000000, 0x0000a0, 0x0080ff, 0x00e0a0, 0x80ff00, 0xffe000, 0xff4000, 0xff0000};
        static uint32_t       preset[256];

        switch (param_i_get(cfg_wf_palette)) {
            case 1: wf_palette = wf_palette_legacy; break;
            case 2: wf_palette = wf_palette_gauss; break;
            case 3: theme_ramp_palette(grey, 2, 0, preset); wf_palette = preset; break;
            case 4: theme_ramp_palette(fire, 7, 0, preset); wf_palette = preset; break;
            case 5: theme_ramp_palette(ice, 6, 0, preset); wf_palette = preset; break;
            case 6: theme_ramp_palette(rainbow, 8, 0, preset); wf_palette = preset; break;
            default: wf_palette = theme_waterfall(); break;
        }
    }
    scheduler_put_coalesced(refresh_waterfall);
}

/* The row age rows before the newest (0: the newest), age under height: never the one the DSP thread
 * is writing */
static wf_row_t *row_at(int32_t age) {
    int32_t ring = height + 1;

    return &rows[(__atomic_load_n(&newest, __ATOMIC_ACQUIRE) - age + ring) % ring];
}

/* As row_at(), from the newest row of one moment: a picture drawn from many rows stays one picture
 * when the DSP thread writes a row meanwhile */
static wf_row_t *row_from(uint16_t top, int32_t age) {
    int32_t ring = height + 1;

    return &rows[(top - age + ring) % ring];
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

static uint8_t  row_seq;    /* 1bit: the next row's dither row */
static uint32_t row_count;  /* rows written: their numbers (3D) */

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
    row->num = ++row_count;
    row->ms = (uint32_t) get_time();

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
 * primary plane shows through whatever is behind in the GUI. Also the spectrum's
 * (spectrum_view.c), but not while it is off: spectrum_layout() parks it under the
 * top bars at OPA_TRANSP, and a hole there showed the primary plane, black, never
 * written there, through their gaps and round corners. Same gate as spectrum_draw_cb() */
void direct_hole_cb(lv_event_t *e) {
    lv_obj_t        *o = lv_event_get_target(e);
    lv_draw_ctx_t   *ctx = lv_event_get_draw_ctx(e);
    lv_area_t       a;
    int32_t         stride = lv_area_get_width(ctx->buf_area);

    if (lv_obj_get_style_opa(o, 0) < LV_OPA_MIN || !_lv_area_intersect(&a, &o->coords, ctx->clip_area)) {
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

    if (!shown(obj) || !backlight_seen()) {
        wfp_full = true;            /* not up to date when shown again (the screen dark, the screensaver over it) */
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

/* ---- 3D: the last rows as traces one behind the other ---- */

/* Settings > Display > WATERFALL, 3D: the newest row in front at the bottom, older ones behind it,
 * each higher up and narrower, towards a vanishing point on the VFO line (it stays straight through
 * them all).
 *
 * The traces glide: each is one row, every D3_STEP-th by its number, and stays that row while it
 * moves back by a fraction of the spacing at each new row (picked by age, the stack stood still and
 * then jumped a whole spacing at every third row). The newest row is in front whatever its number:
 * the live trace.
 *
 * Drawn front to back with a horizon per column (the floating horizon of the old surface plots): a
 * trace shows only where it rises above all those in front of it, down to its own foot. The noise
 * floor is cut away (each row's floor, its 75th percentile, and Floor levels above it): only signals
 * stand up, over a black floor with a grid, a back wall and side walls of the level scale, and lines
 * from the frequency scale's reference frequencies to the vanishing point, labelled at the front.
 * Each pixel has the colour of its height on the strength scale (3D colours), the ridges lighter
 * with their top pixel antialiased, the back darker. The newest trace is drawn over it all as an
 * antialiased line of its own colour, along the front edge where there is nothing, and its peaks
 * held a moment above it (Peaks).
 *
 * Every refresh draws the whole picture; the rows are those of the waterfall, at the frequency of
 * the view now, so tuning and the zoom move the traces as they move the waterfall */
#define D3_STEP     3       /* rows from one trace to the next */
#define MAX_D3_REFS 8
#define D3_LABELS   MAX_D3_REFS
#define D3_LABEL_H  18      /* the front labels' row, under the floor */
#define D3_GRID     0x2c3848
#define D3_WALL     0x222c3a
#define D3_PEAK_DECAY 90.0f /* levels a second the held peaks come down */
#define D3_MIN_MS   33      /* at most a picture so often (30 a second) while tuning; the last one later */
#define D3_CACHE    72      /* rows whose levels are kept (every D3_STEP-th: up to 64 traces and a few) */

enum { D3_STRENGTH, D3_WATERFALL, D3_BLUE };    /* Settings, 3D colours */

static int32_t      d3_vanish = WIDTH / 2;  /* the vanishing point, x: the VFO line (update_middle_line()) */
static uint32_t     d3_ramp[256];
static int32_t      d3_ramp_of = -1;        /* the 3D colours d3_ramp was made for */
static float        d3_peak[WIDTH];         /* held levels of the newest trace, by screen x */
static int32_t      d3_peak_center;         /* the view they were held at */
static uint32_t     d3_peak_span;
static uint32_t     d3_peak_tick;
static lv_obj_t    *d3_labels[D3_LABELS];   /* the reference frequencies at the front edge */
static int32_t      d3_label_hz[D3_LABELS];
static lv_coord_t   d3_label_y = -1;
static uint32_t     d3_last_ms;             /* when the last picture was drawn */
static float        d3_drawn_phase = 1.0f;  /* the phase of the last picture (d3_phase()) */
static lv_timer_t  *d3_frames;              /* moves the traces between rows */
static lv_timer_t  *d3_later;               /* a picture asked for too soon: drawn then */

/* The levels of the traces, kept while the view stays: only a new row's are worked out (row_pixels() and the
 * smoothing were a sixth of a picture's work, at every row for every trace) */
static struct {
    uint32_t num;           /* the row's number; 0: none */
    int32_t  center;        /* the view they are of */
    uint32_t span;
    int32_t  cut;           /* the floor cut they are above */
    int32_t  scale;         /* their height a level above it, in 1/256 (d3_scale()) */
    uint8_t  lv[WIDTH];     /* above the cut, to the whole scale; 0: the floor */
    uint8_t  blk[WIDTH / 8 + 1];    /* an 8 px block has something above the floor (or its next pixel has) */
} d3_cache[D3_CACHE];

/* What the last picture drew over the background in the buffer, by column (lo > hi: nothing): the next one puts
 * back only that from the background, not all of it (its copy was a third of a picture) */
static int16_t      d3_dirty_lo[WIDTH], d3_dirty_hi[WIDTH];
static uint32_t    *d3_drawn_buf;           /* the buffer they are of; NULL: not known (2D drew in it) */
/* Column x drawn from lo to hi over the background */
static inline void d3_dirty(int32_t x, int32_t lo, int32_t hi) {
    if (lo < d3_dirty_lo[x]) d3_dirty_lo[x] = lo;
    if (hi > d3_dirty_hi[x]) d3_dirty_hi[x] = hi;
}


/* The background (the panel, the grid, the walls): the same picture until the scene changes, drawn again then
 * (its antialiased lines, at every picture, were most of the work) */
static uint32_t    *d3_bg;
static struct {
    int32_t  h, hv, cx, tilt, n_refs, ref_x[MAX_D3_REFS];
    uint32_t panel;
    bool     portrait, mono;
} d3_bg_of;

/* The front edge of the floor, with the labels under it: above the buttons, or down to the bottom while they are
 * hidden (Settings > Display > Hide buttons) */
static int32_t d3_front(int32_t h) {
    return LV_MAX(h / 2, h - (buttons_hidden() ? 0 : buttons_h) - 4 - D3_LABEL_H);
}

/* Not under the Amber Trey display (lcd_panel.c), which covers the waterfall: there the 2D one, as before */
static bool d3_on(void) {
    return param_i_get(cfg_wf_3d) && !theme_amber;
}

/* The colours by height and the newest trace's */
static const uint32_t *d3_palette(uint32_t *accent) {
    /* Strength: dark blue at the floor, cyan, green, yellow, orange, red for the strongest */
    static const uint32_t strength[] = {0x001048, 0x0030c0, 0x0090ff, 0x00e0c0, 0x40f040, 0xd8f000, 0xffa000, 0xff3000};
    /* Blue: one hue, brighter with the level */
    static const uint32_t blue[] = {0x000a28, 0x0028a0, 0x1070ff, 0x60b0ff, 0xd8f0ff};
    int32_t c = param_i_get(cfg_wf_3d_colors);

    if (c == D3_WATERFALL) {
        *accent = 0xffffff;
        return wf_palette;
    }
    if (c != d3_ramp_of) {
        if (c == D3_BLUE) {
            theme_ramp_palette(blue, sizeof(blue) / sizeof(blue[0]), 0, d3_ramp);
        } else {
            theme_ramp_palette(strength, sizeof(strength) / sizeof(strength[0]), 0, d3_ramp);
        }
        d3_ramp_of = c;
    }
    *accent = c == D3_BLUE ? 0xe8f6ff : 0xff4020;
    return d3_ramp;
}

/* c at a/256 of its brightness */
static inline uint32_t d3_dim(uint32_t c, uint32_t a) {
    uint32_t rb = ((c & 0x00ff00ffu) * a >> 8) & 0x00ff00ffu;
    uint32_t g = ((c & 0x0000ff00u) * a >> 8) & 0x0000ff00u;

    return rb | g | 0xff000000u;
}

/* c a third of the way to white */
static inline uint32_t d3_light(uint32_t c) {
    uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;

    return (r + (255 - r) / 3) << 16 | (g + (255 - g) / 3) << 8 | (b + (255 - b) / 3);
}

/* src over dst, a of 255 */
static inline uint32_t d3_blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t rb = ((dst & 0x00ff00ffu) * (255u - a) + (src & 0x00ff00ffu) * a + 0x00800080u) >> 8;
    uint32_t g = ((dst & 0x0000ff00u) * (255u - a) + (src & 0x0000ff00u) * a + 0x00008000u) >> 8;

    return (rb & 0x00ff00ffu) | (g & 0x0000ff00u) | 0xff000000u;
}

typedef struct {
    uint32_t *buf;
    bool      portrait;     /* as wfp: landscape x at row WIDTH - 1 - x, y at its column; else the LVGL image */
    int32_t   h;
    bool      mono;         /* 1bit: no partial pixels, the lines dotted */
} d3_target_t;

static inline uint32_t *d3_at(const d3_target_t *o, int32_t x, int32_t y) {
    return o->portrait ? &o->buf[(WIDTH - 1 - x) * o->h + y] : &o->buf[y * WIDTH + x];
}

/* A pixel at coverage a (0..1) */
static inline void d3_px(const d3_target_t *o, int32_t x, int32_t y, uint32_t c, float a) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= o->h || a <= 0.0f) {
        return;
    }
    uint32_t *p = d3_at(o, x, y);

    if (o->mono) {
        if (a >= 0.5f) *p = c | 0xff000000u;
    } else {
        *p = a >= 1.0f ? c | 0xff000000u : d3_blend(*p, c, (uint32_t)(a * 255.0f + 0.5f));
    }
}

/* An antialiased line (Wu's), at opacity op; 1bit: every other pixel of whole ones */
static void d3_line(const d3_target_t *o, float x0, float y0, float x1, float y1, uint32_t c, float op) {
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
    float g = dx == 0.0f ? 0.0f : (y1 - y0) / dx;
    float y = y0 + g * (roundf(x0) - x0);

    for (int32_t x = roundf(x0); x <= roundf(x1); x++, y += g) {
        int32_t yi = floorf(y);
        float   f = y - yi;

        if (o->mono) {
            if (!(x & 1)) {
                if (steep) d3_px(o, lroundf(y), x, c, 1.0f);
                else d3_px(o, x, lroundf(y), c, 1.0f);
            }
        } else if (steep) {
            d3_px(o, yi, x, c, (1.0f - f) * op);
            d3_px(o, yi + 1, x, c, f * op);
        } else {
            d3_px(o, x, yi, c, (1.0f - f) * op);
            d3_px(o, x, yi + 1, c, f * op);
        }
    }
}

/* Column x of a line from the column before: from ya (the column before) to yb, th px thick, its ends
 * antialiased. A line across the screen in one of these a column: Wu's lines of one column each, two calls a
 * column for the newest trace, were most of what was left of a picture */
static void d3_join(const d3_target_t *o, int32_t x, float ya, float yb, float th, uint32_t c, float op) {
    float   lo = LV_MIN(ya, yb), hi = LV_MAX(ya, yb) + th;
    int32_t y0 = LV_MAX(0, (int32_t) floorf(lo)), y1 = LV_MIN(o->h - 1, (int32_t) ceilf(hi) - 1);

    for (int32_t y = y0; y <= y1; y++) {
        float cov = LV_MIN((float)(y + 1), hi) - LV_MAX((float) y, lo);

        d3_px(o, x, y, c, LV_MIN(cov, 1.0f) * op);
    }
    if (y0 <= y1 && x >= 0 && x < WIDTH) {
        d3_dirty(x, y0, y1);
    }
}

/* The scene: from the settings and the height seen */
typedef struct {
    float hv;       /* the height seen: the buttons and the labels' row are over the bottom */
    float amp;      /* the front trace's full scale, px */
    float rise;     /* the back trace's foot this far above the front one's */
    float shrink;   /* the back trace this much as wide as the front one */
    float cx;       /* the vanishing point's x */
    int32_t span;   /* rows from the front to the back */
} d3_scene_t;

/* Depth t (0 in front, 1 at the back): the scale and the foot */
static inline void d3_place(const d3_scene_t *sc, float t, float *s, float *foot) {
    *s = 1.0f - (1.0f - sc->shrink) * t;
    *foot = sc->hv - 2 - t * sc->rise;
}

static inline float d3_x(const d3_scene_t *sc, float fx, float s) {
    return sc->cx + (fx - sc->cx) * s;
}

/* The reference frequencies of the scale on the floor, the floor's edges and lines across it, the
 * back wall and the side walls with the level scale at quarters */
static void d3_grid(const d3_target_t *o, const d3_scene_t *sc, const int32_t *ref_x, int n_refs) {
    uint32_t grid = o->mono ? theme_shape.ink : D3_GRID;
    uint32_t wall = o->mono ? theme_shape.ink : D3_WALL;
    float    s0, f0, s1, f1;

    d3_place(sc, 0, &s0, &f0);
    d3_place(sc, 1, &s1, &f1);
    float l0 = d3_x(sc, 0, s1), r0 = d3_x(sc, WIDTH - 1, s1);   /* the back corners */

    /* Across the floor every quarter of the depth, its front and back edges */
    for (int k = 0; k <= 4; k++) {
        float s, f;

        d3_place(sc, k / 4.0f, &s, &f);
        d3_line(o, d3_x(sc, 0, s), f, d3_x(sc, WIDTH - 1, s), f, grid, k == 0 ? 0.9f : 0.6f);
    }
    /* Along it: the floor's sides and the reference frequencies */
    d3_line(o, 0, f0, l0, f1, grid, 0.8f);
    d3_line(o, WIDTH - 1, f0, r0, f1, grid, 0.8f);
    for (int i = 0; i < n_refs; i++) {
        d3_line(o, ref_x[i], f0, d3_x(sc, ref_x[i], s1), f1, grid, 0.7f);
    }
    /* The back wall and the side walls: the level scale at quarters, the wall's edges */
    float top_b = f1 - sc->amp * s1;

    for (int k = 1; k <= 4; k++) {
        float yb = f1 - sc->amp * s1 * k / 4.0f, yf = f0 - sc->amp * k / 4.0f;

        d3_line(o, l0, yb, r0, yb, wall, 0.8f);
        d3_line(o, 0, yf, l0, yb, wall, 0.5f);
        d3_line(o, WIDTH - 1, yf, r0, yb, wall, 0.5f);
    }
    d3_line(o, l0, f1, l0, top_b, wall, 0.9f);
    d3_line(o, r0, f1, r0, top_b, wall, 0.9f);
}

/* One trace: lv (levels by the view's pixel) at depth t, its horizon kept; the top of each column
 * into tops (the newest), NAN where it is not drawn */
static void d3_trace(const d3_target_t *o, const d3_scene_t *sc, const uint8_t *lv, const uint8_t *blk, float t,
                     const uint32_t *pal, int16_t *horizon, float *tops) {
    float    s, foot;

    d3_place(sc, t, &s, &foot);
    float    a = sc->amp * s / 255.0f;      /* px a level */
    float    per_px = 1.0f / a;             /* levels a px */
    /* The colours of this trace, worked out once each as they are needed (a colour worked out at every pixel of
     * the ribbons was half the picture): the ribbon's by the pixels above the foot, the ridge's by the level */
    int32_t  foot_px = (int32_t) foot;
    float    foot_frac = foot - foot_px;
    uint32_t fill_lut[MAX_HEIGHT + 1];
    int32_t  fill_n = 0;                    /* fill_lut[0 .. fill_n - 1] worked out */
    uint32_t ridge_lut[256];
    uint8_t  ridge_have[256];

    memset(ridge_have, 0, sizeof(ridge_have));
    float    cx = sc->cx;
    float    inv_s = 1.0f / s;
    int32_t  x0 = LV_MAX(0, (int32_t) ceilf(cx - cx * s));
    int32_t  x1 = LV_MIN(WIDTH - 1, (int32_t) floorf(cx + (WIDTH - 1 - cx) * s));
    uint32_t fog = 256 - (uint32_t)(t * 150.0f);    /* the back ones darker */
    int32_t  bottom_max = (int32_t) foot + 1;
    int32_t  step = o->portrait ? 1 : WIDTH;    /* buf from one landscape y to the next */
    uint32_t ink = theme_shape.ink | 0xff000000u, paper = theme_shape.paper | 0xff000000u;
    int32_t  prev = -1;                     /* the top drawn in the column before; -1: none */

    float    u0 = cx + (x0 - cx) * inv_s;  /* the view's pixel under column x, moved on a column at a time */

    for (int32_t x = x0; x <= x1; x++, u0 += inv_s) {
        /* The view's pixels under this one: the highest of them, so a narrow carrier stays at the back */
        int32_t i0 = LV_CLAMP(0, (int32_t) u0, WIDTH - 1);
        int32_t i1 = LV_CLAMP(i0, (int32_t)(u0 + inv_s), WIDTH - 1);
        int32_t v;

        if (!blk[i0 >> 3] && !blk[i1 >> 3]) {
            /* Nothing above the floor there (most of a quiet band): on to the column whose pixels reach the next
             * block with something, all of it the floor (a column at a time, that was most of a trace's work) */
            int32_t b = (i1 >> 3) + 1;

            while (b <= (WIDTH - 1) >> 3 && !blk[b]) {
                b++;
            }
            int32_t xn = b > (WIDTH - 1) >> 3 ? x1 + 1 : (int32_t) ceilf(cx + ((b << 3) - 1 - inv_s - cx) * s);

            xn = LV_CLAMP(x + 1, xn, x1 + 1);
            if (tops) {
                for (int32_t k = x; k < xn; k++) tops[k] = foot;
            }
            prev = -1;
            x = xn - 1;
            u0 = cx + (x - cx) * inv_s;     /* the loop moves both on to xn */
            continue;
        }

        if (i1 > i0 + 1) {
            v = lv[i0];
            for (int32_t i = i0 + 1; i < i1; i++) {
                v = LV_MAX(v, lv[i]);
            }
        } else {
            /* Between two pixels: linear, the front trace's slopes smooth */
            float f = u0 - i0;

            /* rounded by + 0.5 (both are levels, 0 or more): lroundf() is a call of the library */
            v = i0 + 1 < WIDTH ? (int32_t)(lv[i0] + (lv[i0 + 1] - lv[i0]) * f + 0.5f) : lv[i0];
        }
        float   topf = LV_MAX(0.0f, foot - v * a);
        int32_t top = (int32_t) topf;       /* its pixel, covered 1 - (topf - top) */

        if (tops) {
            tops[x] = foot - v * a;
        }
        if (v == 0 || top >= horizon[x]) {
            prev = -1;
            continue;                       /* the floor, or behind the traces in front */
        }
        int32_t   bottom = LV_MIN(horizon[x], bottom_max);
        /* The ridge joined to the column before: down to its top where it is lower, up to it where it is
         * higher (all of that above the horizon: seen) */
        int32_t   r0 = prev >= 0 ? LV_MIN(top + 1, prev) : top + 1;
        int32_t   r1 = prev >= 0 ? LV_MAX(top + 1, LV_MIN(prev, bottom - 1)) : top + 1;
        uint32_t  ridge;

        if (o->mono) {
            ridge = ink;
        } else {
            if (!ridge_have[v]) {
                ridge_lut[v] = d3_dim(d3_light(pal[v]), fog);
                ridge_have[v] = 1;
            }
            ridge = ridge_lut[v];
        }

        if (o->mono) {
            r0 = LV_MIN(r0, top);
            *d3_at(o, x, top) = ink;
        } else {
            d3_px(o, x, top, ridge, 1.0f - (topf - top));
        }
        uint32_t *p = d3_at(o, x, top);

        for (int32_t y = r0; y < top; y++) {     /* up to the column before (its top pixel antialiased) */
            *d3_at(o, x, y) = ridge;
        }
        for (int32_t y = top + 1; y < bottom; y++) {
            p += step;
            if (y <= r1) {
                *p = ridge;
            } else if (o->mono) {
                *p = paper;
            } else {
                int32_t k = foot_px - y;            /* px above the foot */

                if (k < 0) {
                    k = 0;
                }
                while (fill_n <= k) {               /* the colour of the level at that height */
                    int32_t l = (fill_n + foot_frac) * per_px;

                    fill_lut[fill_n++] = d3_dim(pal[LV_CLAMP(0, l, 255)], fog * 3 / 4);
                }
                *p = fill_lut[k];
            }
        }
        horizon[x] = LV_MIN(r0, top);
        d3_dirty(x, horizon[x], bottom - 1);
        prev = top + 1;
    }
}

/* The view's pixels of a row, smoothed 1 4 6 4 1 but never below the pixel itself: the noise a surface, not
 * grains, and a signal of a pixel or two (CW and FT8 at 50 kHz and wider: 62 Hz and more a pixel) at its own
 * height; smoothed alone, a recording of real traffic showed them at four tenths of it */
static void d3_levels(const wf_row_t *row, uint32_t span, uint8_t *lv) {
    uint8_t raw[WIDTH];

    row_pixels(row, wf_center_freq, span, 0, WIDTH, raw);
    for (int32_t i = 0; i < WIDTH; i++) {
        int32_t a = raw[LV_MAX(i - 2, 0)], b = raw[LV_MAX(i - 1, 0)], c = raw[i];
        int32_t d = raw[LV_MIN(i + 1, WIDTH - 1)], e = raw[LV_MIN(i + 2, WIDTH - 1)];

        lv[i] = LV_MAX(c, (a + 4 * b + 6 * c + 4 * d + e + 8) >> 4);
    }
}

/* How far the newest row is to the next, 0 when it came to 1 when the next is due (the rows' interval from the
 * last 8): the traces are drawn that much of a row further back, so they move on at every picture, not at every
 * row. 1 when it is late: no further than where the next row puts them */
static float d3_phase(uint16_t top) {
    const wf_row_t *a = row_from(top, 0), *b = row_from(top, 8);

    if (!param_i_get(cfg_wf_3d_glide)) {
        return 0.0f;                        /* 3D glide off: the traces stay at their rows, a step at each one */
    }
    if (!a->width || !b->width || height <= 9) {
        return 0.0f;
    }
    uint32_t every = (a->ms - b->ms) / 8;
    uint32_t since = (uint32_t) get_time() - a->ms;

    if (every == 0 || every > 1000) {
        return 0.0f;                        /* rows from before a pause, or none yet */
    }
    return LV_MIN(1.0f, (float) since / every);
}

/* The height of a level above the floor cut, in 1/256 of a level of the whole height: 3D range dB above the floor
 * is the whole height. The rows' levels are the waterfall's colours, from its min to its max: their dB a level from
 * the levels now (the scale of the colours is wider, so the colours have room; on it a signal 30 dB above the
 * noise, real traffic on a wire, stood a third of the height) */
static int32_t d3_scale(void) {
    static int32_t held;            /* moved only by more than 4 %: auto level moves a little all the time, and each
                                     * new scale works out the levels of every trace again */
    float          min, max;

    waterfall_get_levels(false, &min, &max);
    int32_t now = LV_CLAMP(64, (int32_t)((max - min) / param_i_get(cfg_wf_3d_range) * 256.0f), 64 * 256);

    if (LV_ABS(now - held) * 25 > held) {
        held = now;
    }
    return held;
}

/* Levels to above the floor cut (cut and below: 0, the rest at scale/256 a level, to the whole height), and where
 * there is something */
static void d3_cut(uint8_t *lv, int32_t cut, int32_t scale, uint8_t *blk) {
    memset(blk, 0, WIDTH / 8 + 1);
    for (int32_t i = 0; i < WIDTH; i++) {
        int32_t v = lv[i] > cut ? LV_MIN(255, ((lv[i] - cut) * scale) >> 8) : 0;

        lv[i] = v;
        if (v) {
            blk[i >> 3] = 1;
            if (i > 0) blk[(i - 1) >> 3] = 1;   /* the pixel before reads this one between them */
        }
    }
}

/* A trace's levels above the floor and its blocks: kept for the rows of the stack (every D3_STEP-th), worked
 * out for the newest */
static const uint8_t *d3_levels_of(const wf_row_t *row, uint32_t span, int32_t cut, int32_t scale, uint8_t *scratch,
                                   uint8_t *scratch_blk, const uint8_t **blk) {
    if (row->num % D3_STEP) {
        d3_levels(row, span, scratch);
        d3_cut(scratch, cut, scale, scratch_blk);
        *blk = scratch_blk;
        return scratch;
    }
    typeof(d3_cache[0]) *c = &d3_cache[row->num / D3_STEP % D3_CACHE];

    if (c->num != row->num || c->center != wf_center_freq || c->span != span || c->cut != cut || c->scale != scale) {
        d3_levels(row, span, c->lv);
        d3_cut(c->lv, cut, scale, c->blk);
        c->num = row->num;
        c->center = wf_center_freq;
        c->span = span;
        c->cut = cut;
        c->scale = scale;
    }
    *blk = c->blk;
    return c->lv;
}

/* The background into buf: again from the scene when it changed */
static void d3_background(const d3_target_t *o, const d3_scene_t *sc, uint32_t bg, int32_t tilt,
                          const int32_t *ref_x, int n_refs) {
    size_t n = (size_t) WIDTH * o->h;
    bool   same = d3_bg && d3_bg_of.h == o->h && d3_bg_of.hv == (int32_t) sc->hv &&
                  d3_bg_of.cx == (int32_t) sc->cx && d3_bg_of.tilt == tilt && d3_bg_of.panel == bg &&
                  d3_bg_of.portrait == o->portrait && d3_bg_of.mono == o->mono &&
                  d3_bg_of.n_refs == n_refs && !memcmp(d3_bg_of.ref_x, ref_x, n_refs * sizeof(int32_t));

    if (!d3_bg) {
        d3_bg = malloc(sizeof(uint32_t) * WIDTH * MAX_HEIGHT);
    }
    if (!same && d3_bg) {
        d3_target_t b = *o;

        b.buf = d3_bg;
        for (size_t i = 0; i < n; i++) {
            d3_bg[i] = bg;
        }
        d3_grid(&b, sc, ref_x, n_refs);
        d3_bg_of.h = o->h;
        d3_bg_of.hv = sc->hv;
        d3_bg_of.cx = sc->cx;
        d3_bg_of.tilt = tilt;
        d3_bg_of.panel = bg;
        d3_bg_of.portrait = o->portrait;
        d3_bg_of.mono = o->mono;
        d3_bg_of.n_refs = n_refs;
        memcpy(d3_bg_of.ref_x, ref_x, n_refs * sizeof(int32_t));
    }
    if (d3_bg && same && d3_drawn_buf == o->buf) {
        /* Only what the last picture drew over it */
        for (int32_t x = 0; x < WIDTH; x++) {
            int32_t lo = d3_dirty_lo[x], hi = LV_MIN(d3_dirty_hi[x], o->h - 1);

            if (lo > hi) {
                continue;
            }
            if (o->portrait) {
                size_t at = (size_t)(WIDTH - 1 - x) * o->h + lo;

                memcpy(o->buf + at, d3_bg + at, (hi - lo + 1) * sizeof(uint32_t));
            } else {
                for (int32_t y = lo; y <= hi; y++) {
                    o->buf[y * WIDTH + x] = d3_bg[y * WIDTH + x];
                }
            }
        }
    } else if (d3_bg) {
        memcpy(o->buf, d3_bg, n * sizeof(uint32_t));
    } else {                                /* no memory for it: drawn here */
        for (size_t i = 0; i < n; i++) {
            o->buf[i] = bg;
        }
        d3_grid(o, sc, ref_x, n_refs);
    }
    for (int32_t x = 0; x < WIDTH; x++) {
        d3_dirty_lo[x] = INT16_MAX;
        d3_dirty_hi[x] = -1;
    }
    d3_drawn_buf = d3_bg ? o->buf : NULL;
}


__attribute__((noinline))   /* one picture: measurable on its own */
static void d3_render(uint32_t *buf, bool portrait, const int32_t *ref_x, int n_refs) {
    d3_target_t     o = {buf, portrait, height, theme_shape.mono};
    int32_t         h = height;
    uint32_t        span = shown_span();
    uint32_t        accent;
    const uint32_t *pal = d3_palette(&accent);
    uint32_t        bg = o.mono ? theme_shape.paper | 0xff000000u : TC(panel).full | 0xff000000u;
    int32_t         tilt = param_i_get(cfg_wf_3d_tilt);   /* 0 flat on .. 10 seen from above */
    int32_t         cut_add = param_i_get(cfg_wf_3d_floor);
    int32_t         scale = d3_scale();
    d3_scene_t      sc;
    int16_t         horizon[WIDTH];         /* the highest pixel drawn in each column: none, the bottom */
    float           front[WIDTH];           /* the newest trace's top in each column */
    uint8_t         lv[WIDTH], lv_blk[WIDTH / 8 + 1];

    sc.hv = d3_front(h);
    sc.amp = sc.hv * (0.75f - 0.04f * tilt);
    sc.rise = sc.hv - sc.amp - 4;
    sc.shrink = 0.9f - 0.05f * tilt;
    sc.cx = d3_vanish;
    sc.span = LV_MAX(1, LV_MIN(param_i_get(cfg_wf_3d_depth) * D3_STEP, h - 1));

    d3_background(&o, &sc, bg, tilt, ref_x, n_refs);
    for (int32_t x = 0; x < WIDTH; x++) {
        horizon[x] = h;
        front[x] = NAN;
    }

    /* The newest in front, then each D3_STEP-th row by its number, at its age's depth; all from the newest of
     * this moment */
    uint16_t        top = __atomic_load_n(&newest, __ATOMIC_ACQUIRE);
    const wf_row_t *newest_row = row_from(top, 0);
    float           phase = d3_phase(top);

    for (int32_t age = 0; age <= sc.span; age++) {
        const wf_row_t *row = row_from(top, age);
        float           t = (age + phase) / sc.span;

        if (!row->width || (age > 0 && row->num % D3_STEP) || t > 1.0f) {
            continue;
        }
        const uint8_t *blk;
        const uint8_t *levels = d3_levels_of(row, span, LV_MIN(row->floor + cut_add, 254), scale, lv, lv_blk, &blk);

        d3_trace(&o, &sc, levels, blk, t, pal, horizon, age == 0 ? front : NULL);
    }
    d3_drawn_phase = phase;
    if (!newest_row->width) {
        return;
    }

    /* The held peaks of the newest trace, a thin line over it; again from it on a new view */
    float s0, f0;

    d3_place(&sc, phase / sc.span, &s0, &f0);
    uint32_t now = lv_tick_get();
    float    fall = (now - d3_peak_tick) / 1000.0f * D3_PEAK_DECAY * sc.amp / 255.0f;   /* px */
    bool     peaks = param_i_get(cfg_wf_3d_peak);

    if (d3_peak_center != wf_center_freq || d3_peak_span != span || fall > sc.amp) {
        for (int32_t x = 0; x < WIDTH; x++) d3_peak[x] = f0;
        d3_peak_center = wf_center_freq;
        d3_peak_span = span;
        fall = 0;
    }
    d3_peak_tick = now;
    for (int32_t x = 0; x < WIDTH; x++) {
        float y = isnan(front[x]) ? f0 : front[x];

        d3_peak[x] = LV_MIN(LV_MIN(d3_peak[x] + fall, f0), y);
    }
    if (peaks) {
        uint32_t pc = o.mono ? theme_shape.ink : d3_light(accent & 0xffffff);

        for (int32_t x = 1; x < WIDTH; x++) {
            if (d3_peak[x] < f0 - 1 || d3_peak[x - 1] < f0 - 1) {
                d3_join(&o, x, d3_peak[x - 1], d3_peak[x], 1.0f, pc, 0.55f);
            }
        }
    }

    /* The newest trace over it all: 2 px, antialiased, its columns joined */
    uint32_t lc = o.mono ? theme_shape.ink : accent & 0xffffff;

    for (int32_t x = 0; x < WIDTH; x++) {
        if (isnan(front[x])) {
            continue;
        }
        d3_join(&o, x, x > 0 && !isnan(front[x - 1]) ? front[x - 1] : front[x], front[x], 2.0f, lc, 1.0f);
    }
}

/* The reference frequencies under the front edge of the floor; hidden out of 3D */
static void d3_labels_update(bool on, const int32_t *ref_x, const int32_t *ref_hz, int n, lv_coord_t y) {
    for (int i = 0; i < D3_LABELS; i++) {
        if (!d3_labels[i]) {
            d3_labels[i] = lv_label_create(obj);
            lv_obj_set_style_text_font(d3_labels[i], &sony_16, 0);
            lv_obj_set_style_text_color(d3_labels[i], TC(text_dim), 0);
            lv_obj_set_width(d3_labels[i], 80);
            lv_obj_set_style_text_align(d3_labels[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(d3_labels[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(d3_labels[i], LV_OBJ_FLAG_HIDDEN);
            d3_label_hz[i] = -1;
        }
        bool show = on && i < n;

        if (show) {
            if (d3_label_hz[i] != ref_hz[i]) {
                uint16_t mhz, khz, hz;

                split_freq(ref_hz[i], &mhz, &khz, &hz);
                lv_label_set_text_fmt(d3_labels[i], "%i.%03i", mhz, khz);
                d3_label_hz[i] = ref_hz[i];
            }
            if (lv_obj_get_x(d3_labels[i]) != ref_x[i] - 40 || y != d3_label_y) {
                lv_obj_set_pos(d3_labels[i], ref_x[i] - 40, y);
            }
        }
        if (show == lv_obj_has_flag(d3_labels[i], LV_OBJ_FLAG_HIDDEN)) {
            if (show) lv_obj_clear_flag(d3_labels[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(d3_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    d3_label_y = y;
}

static void d3_redraw(void);

/* Whether a picture is seen: not under a window (the waterfall a refresh in refresh_period then, as 2D, without
 * the glide's pictures between), not in the walkie screen (the main screen is not loaded, not hidden), not with the
 * screen dark (unless the remote screen is watched) */
static bool d3_seen(void) {
    return shown(obj) && refresh_period == 1 && !walkie_on() && backlight_seen();
}

static void d3_later_cb(lv_timer_t *t) {
    d3_later = NULL;                /* a single run: LVGL deletes it */
    if (d3_on() && d3_seen()) {
        d3_redraw();
    }
}

/* Every D3_MIN_MS while the traces are on their way to the next row; paused once they are there (no rows: still),
 * with 3D off, glide off or nothing seen. A picture from a new row starts it again */
static void d3_frames_cb(lv_timer_t *t) {
    if (d3_on() && param_i_get(cfg_wf_3d_glide) && d3_drawn_phase < 1.0f && d3_seen()) {
        d3_redraw();
    } else {
        lv_timer_pause(t);
    }
}

static void d3_redraw(void) {
    int32_t  ref_x[MAX_D3_REFS], ref_hz[MAX_D3_REFS];
    int      n = freq_scale_refs(ref_x, ref_hz, MAX_D3_REFS);
    uint32_t since = lv_tick_elaps(d3_last_ms);

    if (!shown(obj) || walkie_on() || !backlight_seen()) {
        d3_drawn_buf = NULL;        /* the plane may have been drawn over: the whole background again */
        return;
    }
    /* Tuning asks at each step of the scroll, 50 a second: the whole picture each time was a third of a core. At
     * most one in D3_MIN_MS, and one at its end so the last step is drawn */
    if (since < D3_MIN_MS) {
        if (!d3_later) {
            d3_later = lv_timer_create(d3_later_cb, D3_MIN_MS - since, NULL);
            lv_timer_set_repeat_count(d3_later, 1);
        }
        return;
    }
    d3_last_ms = lv_tick_get();
    if (param_i_get(cfg_wf_3d_glide) && d3_seen()) {
        if (!d3_frames) {
            d3_frames = lv_timer_create(d3_frames_cb, D3_MIN_MS, NULL);
        } else {
            lv_timer_resume(d3_frames);  /* pauses itself when there is nothing to move */
        }
    }
    __atomic_store_n(&new_rows, 0, __ATOMIC_RELEASE);
    wfp_full = true;                /* the waterfall again whole when 3D is switched off: the plane, or the image */
    if (direct) {
        lv_area_t a;

        d3_render(wfp, true, ref_x, n);
        wfp_head = 0;
        lv_obj_get_coords(obj, &a);
#if USE_DRM
        drm_wf_present_ring(wfp, a.y1, height, wfp_head);
#endif
    } else {
        d3_render((uint32_t *) frame->data, false, ref_x, n);
        lv_obj_invalidate(img);
    }
    lv_coord_t hv = d3_front(height);

    d3_labels_update(true, ref_x, ref_hz, n, hv);
}

/* ---- No planes: LVGL image, scrolled like the DRM path ---- */

/* Pixels i0..i1-1 of landscape row age (0 = newest at the top) */
static void image_paint_line(int32_t age, uint32_t center, uint32_t span, size_t i0, size_t i1, int x0) {
    lv_color_t     *px = (lv_color_t *) frame->data + (size_t) age * WIDTH;
    uint8_t         c[WIDTH];
    const wf_row_t *row = row_at(age);

    row_pixels(row, center, span, i0, i1, c);
    for (size_t i = i0; i < i1; i++) {
        px[i] = (lv_color_t) wf_pixel(c[i], x0 + (int) i, row);
    }
}

static void image_redraw(int new_row) {
    uint32_t span = shown_span();
    int      x0;
    bool     full;
    int32_t  px = 0;

    if (!frame || !shown(obj)) {
        wfp_full = true;            /* not up to date when shown again */
        return;
    }
    if (!backlight_seen()) {
        wfp_full = true;            /* all of it again from the next refresh seen */
        return;
    }
    /* Same idea as direct_redraw(): new lines scroll the bitmap; tuning slides it
     * by whole pixels and only the strip coming in is recomputed. A full rebuild
     * every refresh was the cost on fbdev and the host build. */
    full = wfp_full || new_row >= height || wfp_bw != span;
    if (!full && wfp_center != wf_center_freq) {
        px   = ((int64_t) wf_center_freq - wfp_center) * WIDTH / (int64_t) span;
        full = LV_ABS(px) >= WIDTH / 2;
    }
    if (full) {
        wfp_center = wf_center_freq;
        wfp_bw = span;
        x0 = theme_shape.mono && span ? (int) ((int64_t) wfp_center * WIDTH / span) : 0;
        for (int32_t y = 0; y < height; y++) {
            image_paint_line(y, wfp_center, span, 0, WIDTH, x0);
        }
        wfp_full = false;
    } else if (px || new_row) {
        if (new_row) {
            lv_color_t *data = (lv_color_t *) frame->data;

            /* Newest at the top: scroll older lines down, then paint the new ones */
            if (new_row < height) {
                memmove(data + (size_t) new_row * WIDTH, data,
                        (size_t) (height - new_row) * WIDTH * sizeof(lv_color_t));
            }
            x0 = theme_shape.mono && span ? (int) ((int64_t) wfp_center * WIDTH / span) : 0;
            for (int32_t y = 0; y < new_row; y++) {
                image_paint_line(y, wfp_center, span, 0, WIDTH, x0);
            }
        }
        if (px) {
            size_t      i0, i1;
            lv_color_t *data = (lv_color_t *) frame->data;

            wfp_center += (int64_t) px * span / WIDTH;
            x0 = theme_shape.mono && span ? (int) ((int64_t) wfp_center * WIDTH / span) : 0;
            if (px > 0) {
                for (int32_t y = 0; y < height; y++) {
                    lv_color_t *row = data + (size_t) y * WIDTH;

                    memmove(row, row + px, (size_t) (WIDTH - px) * sizeof(lv_color_t));
                }
                i0 = WIDTH - (size_t) px;
                i1 = WIDTH;
            } else {
                for (int32_t y = 0; y < height; y++) {
                    lv_color_t *row = data + (size_t) y * WIDTH;

                    memmove(row - px, row, (size_t) (WIDTH + px) * sizeof(lv_color_t));
                }
                i0 = 0;
                i1 = (size_t) (-px);
            }
            for (int32_t y = 0; y < height; y++) {
                image_paint_line(y, wfp_center, span, i0, i1, x0);
            }
        }
    } else if (wfp_center == wf_center_freq) {
        return;
    }
    /* Also with only a move under a pixel to go, which waits as on the planes: the image is drawn all the same, as
     * its draw takes the next step of the scroll (image_drawn_cb(); direct mode has a timer for it). Not drawn, each
     * of a smooth slide's last steps waited for the next line */
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
    freq_scale_lines_hide(d3_on());     /* 3D: the reference frequencies on its floor instead */
    if (d3_on()) {
        d3_redraw();
        return;
    }
    if (d3_frames) {
        lv_timer_pause(d3_frames);  /* out of 3D: no glide */
    }
    if (d3_label_y >= 0) {
        d3_labels_update(false, NULL, NULL, 0, 0);     /* out of 3D: its labels away */
        d3_label_y = -1;
    }
    d3_drawn_buf = NULL;            /* 2D draws over it: 3D again from the whole background */
    {
        int n = __atomic_exchange_n(&new_rows, 0, __ATOMIC_ACQUIRE);

        if (direct) {
            direct_redraw(n);
        } else {
            image_redraw(n);
        }
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

void waterfall_redraw() {
    refresh_count = refresh_period;     /* not left out under a dialog */
    scheduler_put_coalesced(refresh_waterfall);
}

/* ---- Where the picture is centred ---- */

/* The passband, Hz from the VFO line: from the VFO up (USB), down (LSB) or both ways (AM, FM), as on
 * the spectrum */
static void line_passband(int32_t *from, int32_t *to) {
    int32_t low = subject_i_get((SubjectInt*)cfg_cur_filter_low);
    int32_t high = subject_i_get((SubjectInt*)cfg_cur_filter_high);
    int32_t tone = param_i_get(cfg_key_tone);
    radio_mode_t mode = subject_i_get((SubjectInt*)cfg_cur_mode);

    switch (mode) {
        case radio_mode_lsb:
        case radio_mode_lsb_dig:
            *from = -high;
            *to = -low;
            break;
        /* CW and CWR: the edges are tones, and the signal heard at the key tone is on the line, so
         * the band is around the line in both, as the spectrum draws it from its CW oscillator (#34: it
         * was beside it, a pitch off, above in CW and below in CWR). From the key tone, not from the
         * LO offset that places the rows (the X6200's is 0: its spectrum is centred on the dial). Where
         * the tone rises as the dial goes up (CW-R; CW too on the X6200, #115) it is heard below an
         * oscillator the key tone above the dial: a low edge at 0 leaves the band off centre that way */
        case radio_mode_cw:
        case radio_mode_cwr:
            if (radio_cw_tone_dir(mode) > 0) {
                *from = tone - high;
                *to = tone - low;
            } else {
                *from = low - tone;
                *to = high - tone;
            }
            break;
        case radio_mode_am:
        case radio_mode_nfm:
        case radio_mode_sam:
        case radio_mode_wfm:
            *from = -high;
            *to = high;
            break;
        default:
            *from = low;
            *to = high;
            break;
    }
}

/* Fixed mode, Settings > Display > WATERFALL (#10): Jump at, and Jump to */
enum { JUMP_AT_40, JUMP_AT_PASSBAND, JUMP_AT_LINE };
enum { JUMP_TO_CENTER, JUMP_TO_EDGE, JUMP_TO_SCROLL };
#define EDGE_HZ     100     /* Jump at Passband and Line: this far from the screen's edge */

/* Where the line may be, Hz from the centre of the view, before the view moves on. 40%: the
 * received data (centred shift below the line) and the line within 40% of the span of the centre.
 * The line goes most of the way to the edge first, and the side tuned away from shows the last
 * picture received there; v0.3.7 jumped at a quarter for the data, to show less of that, and the
 * reporter found it jumped too soon. The line's 40%: an IF shift keeps it up to that far from the
 * data (on_if_shift_zoom(), screen_main.c), and under it the view would follow it at every step.
 * Line: the line EDGE_HZ in from the screen's edges. Passband: the line and the passband
 * whole on the screen, EDGE_HZ in from its edges (the passband alone left the line on the screen's
 * last pixel in USB; the two wider than the screen: as Line) */
static void line_room(int32_t half, int32_t shift, int32_t *lo, int32_t *hi) {
    int32_t jump = param_i_get(cfg_wf_fixed_jump);

    if (jump == JUMP_AT_40) {
        int32_t at = half * 8 / 10;

        *lo = LV_MAX(-at, shift - at);
        *hi = LV_MIN(at, shift + at);
        return;
    }
    *lo = -half + EDGE_HZ;
    *hi = half - EDGE_HZ;
    if (jump == JUMP_AT_PASSBAND) {
        int32_t from, to;

        line_passband(&from, &to);
        from = LV_MIN(from, 0);     /* the line and its passband, Hz from the line */
        to = LV_MAX(to, 0);
        if (to - from <= *hi - *lo) {
            *lo -= from;
            *hi -= to;
        }
    }
}

int32_t waterfall_view_shift(void) {
    static int32_t last_vfo;   /* at the call before: still the same, the limits moved (zoom, filter, mode) */

    if (!param_i_get(cfg_wf_mode)) {
        view_fixed = false;
        return 0;
    }
    int32_t shift = param_i_get(cfg_band_if_shift);
    int32_t vfo = cparam_i_get(cfg_fg_freq);
    int32_t rc = vfo - shift;
    int32_t half = SPAN_HZ / 2 / param_i_get(cfg_mode_zoom);
    int32_t x = vfo - view_center;      /* the line, Hz from the centre of the view */
    int32_t lo, hi;
    bool    moved = vfo != last_vfo;

    last_vfo = vfo;
    line_room(half, shift, &lo, &hi);
    if (!view_fixed || x < lo || x > hi) {
        /* Center: on the received data, the line where Centered mode has it; at an edge, inside the
         * room (40%: the room is around the data). Tuned out by less than the room again, not a band,
         * a frequency typed in or another zoom: Other edge puts the line at the edge it came from,
         * Scroll keeps it at the one it went out of, the view moving along with each step */
        int32_t to = param_i_get(cfg_wf_fixed_jump) == JUMP_AT_40 ? shift : LV_CLAMP(lo, shift, hi);
        int32_t after = param_i_get(cfg_wf_fixed_after);
        bool    stepped = view_fixed && moved && x >= lo - (hi - lo) && x <= hi + (hi - lo);

        if (stepped && after == JUMP_TO_EDGE) {
            to = x > hi ? lo : hi;
        } else if (stepped && after == JUMP_TO_SCROLL) {
            to = x > hi ? hi : lo;
        }
        view_center = vfo - to;
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
    /* 3D: towards the VFO line, straight through the stack; in Fixed mode the line moves across a still view, and
     * the stack with it at every step: there the view's centre */
    d3_vanish = param_i_get(cfg_wf_mode) ? WIDTH / 2 : LV_CLAMP(0, WIDTH / 2 + vfo_x, WIDTH - 1);
    lv_style_set_line_width(&vfo_line_style, width);

    /* The passband, same scale as the line */
    int32_t from, to;

    line_passband(&from, &to);
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
    for (int i = 0; i < D3_LABELS; i++) {
        d3_labels[i] = NULL;        /* the old ones went with the old waterfall (a new theme) */
    }
    d3_label_y = -1;
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
    subject_subscribe_delayed((Subject *)cfg_wf_3d, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_3d_colors, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_3d_depth, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_3d_tilt, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_3d_floor, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_3d_peak, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_3d_glide, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_3d_range, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_fixed_jump, on_view_changed, NULL);
    subject_subscribe_delayed((Subject *)cfg_wf_fixed_after, on_view_changed, NULL);
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
        d3_drawn_buf = NULL;
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

/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The 3D waterfall's close-up (closeup_view.h)
 */

#include "closeup_view.h"
#include "closeup_est.h"

#include "cfg/cfg_api.h"
#include "gui_styles.h"
#include "mono.h"
#include "radio.h"
#include "theme.h"
#include "waterfall_view.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH       800
#define CU_ROWS     (480 / 4 + 1)   /* the strip's rows at most: a quarter of the tallest waterfall, and one written */
#define CU_MIN_H    24

typedef struct {
    float    db[CLOSEUP_BINS];
    int64_t  center;            /* Hz, absolute */
    uint32_t width;             /* Hz the bins cover; 0: empty */
    int32_t  pb_half;
    int32_t  dial;              /* the dial from the centre, Hz */
    int8_t   sideband;          /* 1 USB, -1 LSB, 0 another mode */
} cu_row_t;

static cu_row_t  rows[CU_ROWS];
static uint16_t  newest;                /* the last row written: the DSP thread writes the next, then moves this */
static uint32_t  written;               /* rows written, counted */
static uint32_t  span_now;              /* the span asked of the DSP (closeup_span()) */

static uint32_t  img[WIDTH * (CU_ROWS - 1)];   /* the strip, landscape, as last painted */
static int32_t   img_h;
static uint32_t  img_span;
static int64_t   img_center;
static uint32_t  img_written;
static const uint32_t *img_pal;
static bool      img_mono;

static lv_obj_t *span_label;
static lv_obj_t *off_label;             /* "off -150 Hz", "centred" */
static closeup_est_t est;
static uint32_t  est_tick;              /* when the last row came (lv_tick_get()) */
#define CU_LABEL_MS 500                 /* no rows for this long (TX, a pause): the label away */
#define CU_VOICE_EDGE 300               /* Hz from the dial where a voice begins, SSB tuned right */
static lv_obj_t *parent_obj;

/* ---- DSP thread ---- */

uint32_t closeup_span(void) {
    return __atomic_load_n(&span_now, __ATOMIC_ACQUIRE);
}

void closeup_data(const float *db, uint16_t n, uint32_t width_hz, int64_t center_hz, int32_t pb_half_hz,
                  int32_t dial_hz, int sideband) {
    uint16_t  next = (__atomic_load_n(&newest, __ATOMIC_ACQUIRE) + 1) % CU_ROWS;
    cu_row_t *r = &rows[next];

    if (n != CLOSEUP_BINS) {
        return;
    }
    memcpy(r->db, db, sizeof(r->db));
    r->center = center_hz;
    r->width = width_hz;
    r->pb_half = pb_half_hz;
    r->dial = dial_hz;
    r->sideband = sideband;
    __atomic_store_n(&newest, next, __ATOMIC_RELEASE);
    __atomic_add_fetch(&written, 1, __ATOMIC_RELEASE);
}

/* ---- LVGL thread ---- */

static bool shown(void) {
    return param_i_get(cfg_wf_3d) && wf_closeup_span_hz(param_i_get(cfg_wf_closeup)) &&
           !(radio_caps() & HW_CAP_PSD);
}

lv_coord_t closeup_height(lv_coord_t wf_height) {
    lv_coord_t h = shown() ? LV_MAX(CU_MIN_H, wf_height / 4) : 0;

    h = LV_MIN(h, CU_ROWS - 1);
    __atomic_store_n(&span_now, h ? wf_closeup_span_hz(param_i_get(cfg_wf_closeup)) : 0, __ATOMIC_RELEASE);
    return h;
}

void closeup_pause(void) {
    __atomic_store_n(&span_now, 0, __ATOMIC_RELEASE);
}

void closeup_init(lv_obj_t *parent) {
    parent_obj = parent;
    span_label = lv_label_create(parent);
    lv_obj_set_style_text_font(span_label, &sony_14, 0);
    lv_obj_set_style_text_color(span_label, TC(text_dim), 0);
    lv_obj_clear_flag(span_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(span_label, LV_OBJ_FLAG_HIDDEN);

    off_label = lv_label_create(parent);
    lv_obj_set_style_text_font(off_label, &sony_16, 0);
    lv_obj_set_style_bg_color(off_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(off_label, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(off_label, 4, 0);
    lv_obj_clear_flag(off_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(off_label, LV_OBJ_FLAG_HIDDEN);
    closeup_est_reset(&est);
}

/* The label: how far the strongest signal in the passband is from the strip's middle (where it should be: the
 * passband's middle, the CW signal's own frequency in CW), green when centred; away without a signal or rows */
static void off_label_update(int32_t strip_h) {
    if (!off_label) {
        return;
    }
    if (!strip_h || !est.valid || lv_tick_elaps(est_tick) > CU_LABEL_MS) {
        lv_obj_add_flag(off_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int32_t off = (int32_t) lroundf(est.off_hz);
    bool    centred = LV_ABS(off) <= CLOSEUP_GREEN_HZ;
    char    s[24];

    if (centred) {
        lv_snprintf(s, sizeof(s), "centred");
    } else {
        lv_snprintf(s, sizeof(s), "off %+d Hz", (int) off);
    }
    lv_label_set_text(off_label, s);
    if (theme_shape.mono) {
        lv_obj_set_style_text_color(off_label, lv_color_hex(theme_shape.paper), 0);
        lv_obj_set_style_bg_color(off_label, lv_color_hex(theme_shape.ink), 0);
        lv_obj_set_style_bg_opa(off_label, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_text_color(off_label, lv_color_hex(centred ? 0x40e070 : 0xffcf4a), 0);
    }
    lv_obj_set_pos(off_label, WIDTH / 2 + 8, LV_MAX(0, strip_h - 22));
    lv_obj_clear_flag(off_label, LV_OBJ_FLAG_HIDDEN);
}

static inline const cu_row_t *row_at(uint16_t top, int32_t age) {
    return &rows[(top + CU_ROWS - age) % CU_ROWS];
}

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t rb = ((dst & 0x00ff00ffu) * (255u - a) + (src & 0x00ff00ffu) * a + 0x00800080u) >> 8;
    uint32_t g = ((dst & 0x0000ff00u) * (255u - a) + (src & 0x0000ff00u) * a + 0x00008000u) >> 8;

    return (rb & 0x00ff00ffu) | (g & 0x0000ff00u) | 0xff000000u;
}

/* The colours' scale: the waterfall's (the DSP put the rows on it) */
static void levels(float *lo, float *k) {
    float min, max;

    waterfall_get_levels(false, &min, &max);
    *lo = min;
    *k = 255.0f / LV_MAX(max - min, 10.0f);
}

/* One row of the strip at line y, centred on center over span */
static void paint_line(const cu_row_t *r, int32_t y, int64_t center, uint32_t span, float lo, float k) {
    uint32_t *out = &img[y * WIDTH];
    uint32_t  bg = img_mono ? theme_shape.paper | 0xff000000u : img_pal[0] | 0xff000000u;

    if (!r->width) {
        for (int32_t x = 0; x < WIDTH; x++) out[x] = bg;
        return;
    }
    /* The row's bin under each pixel: from the strip's frequency of the pixel, linear between two bins */
    float bin_per_hz = (float) CLOSEUP_BINS / r->width;
    float b = ((float)(center - r->center) - span / 2.0f) * bin_per_hz + CLOSEUP_BINS / 2.0f;
    float db_step = (float) span / WIDTH * bin_per_hz;
    const uint8_t *density = img_mono ? theme_wf_density() : NULL;

    for (int32_t x = 0; x < WIDTH; x++, b += db_step) {
        int32_t i = (int32_t) b;

        if (b < 0 || i + 1 >= CLOSEUP_BINS) {
            out[x] = bg;
            continue;
        }
        float   f = b - i;
        float   d = r->db[i] + (r->db[i + 1] - r->db[i]) * f;
        int32_t v = LV_CLAMP(0, (int32_t)((d - lo) * k), 255);

        if (img_mono) {
            out[x] = (mono_dither(density[v], x, y) ? theme_shape.ink : theme_shape.paper) | 0xff000000u;
        } else {
            out[x] = img_pal[v] | 0xff000000u;
        }
    }
}

/* The passband shaded, its edges and the centre: where the signal should be */
static void paint_marks(int32_t y0, int32_t y1, int32_t pb_half, uint32_t span) {
    uint32_t line = img_mono ? theme_shape.ink : lv_color_to32(TC(vfo_line)) & 0xffffff;
    int32_t  half_px = (int32_t)((int64_t) pb_half * WIDTH / span);
    int32_t  e0 = LV_CLAMP(0, WIDTH / 2 - half_px, WIDTH - 1), e1 = LV_CLAMP(0, WIDTH / 2 + half_px, WIDTH - 1);

    for (int32_t y = y0; y < y1; y++) {
        uint32_t *out = &img[y * WIDTH];

        if (!img_mono) {
            for (int32_t x = e0; x <= e1; x++) out[x] = blend(out[x], line, 40);
            out[e0] = blend(out[e0], line, 160);
            out[e1] = blend(out[e1], line, 160);
        } else if (y & 1) {
            out[e0] = out[e1] = line | 0xff000000u;
        }
        out[WIDTH / 2] = line | 0xff000000u;
    }
}

static void copy_out(uint32_t *buf, bool portrait, int32_t stride, int32_t h, int32_t y0, int32_t y1) {
    if (!portrait) {
        memcpy(buf + y0 * WIDTH, img + y0 * WIDTH, (size_t)(y1 - y0) * WIDTH * sizeof(uint32_t));
        return;
    }
    for (int32_t x = 0; x < WIDTH; x++) {
        uint32_t *col = buf + (size_t)(WIDTH - 1 - x) * stride;

        for (int32_t y = y0; y < y1 && y < h; y++) col[y] = img[y * WIDTH + x];
    }
}

void closeup_paint(uint32_t *buf, bool portrait, int32_t stride, int32_t strip_h, bool fresh) {
    if (!strip_h) {
        if (span_label) lv_obj_add_flag(span_label, LV_OBJ_FLAG_HIDDEN);
        off_label_update(0);
        img_h = 0;
        return;
    }
    uint16_t        top = __atomic_load_n(&newest, __ATOMIC_ACQUIRE);
    uint32_t        n_written = __atomic_load_n(&written, __ATOMIC_ACQUIRE);
    const cu_row_t *r0 = row_at(top, 0);
    uint32_t        span = closeup_span();
    int64_t         center = r0->width ? r0->center : img_center;
    bool            whole = strip_h != img_h || span != img_span || center != img_center || img_pal != wf_palette ||
                            img_mono != theme_shape.mono;
    uint32_t        news = n_written - img_written;

    if (!span) {
        return;
    }
    /* The estimate from the new rows; again from the start on another frequency */
    if (center != img_center) {
        closeup_est_reset(&est);
    }
    for (int32_t a = (int32_t) LV_MIN(news, (uint32_t) CU_ROWS - 1) - 1; a >= 0; a--) {
        const cu_row_t *r = row_at(top, a);

        if (r->width && r->center == center) {
            /* CW, AM, a carrier: its middle on the strip's (the passband's middle, a CW signal's own frequency);
             * SSB: the voice's edge 300 Hz from the dial (its energy lies low: the middle read hundreds of Hz off
             * when tuned right) */
            int32_t want = r->sideband ? r->dial + r->sideband * CU_VOICE_EDGE : 0;

            closeup_est_update(&est, r->db, CLOSEUP_BINS, r->width, -r->pb_half, r->pb_half, want, r->sideband);
            est_tick = lv_tick_get();
        }
    }
    off_label_update(strip_h);
    if (!whole && !news && !fresh) {
        return;
    }
    img_pal = wf_palette;
    img_mono = theme_shape.mono;

    float lo, k;

    levels(&lo, &k);
    /* The lines worked out: all of them, or only the new ones at the top with the rest moved down by them (not
     * retuned: the old lines stand where they were); the marks over the new lines, the moved ones carry theirs */
    int32_t new_lines = whole || news >= (uint32_t) strip_h ? strip_h : (int32_t) news;

    if (new_lines < strip_h && new_lines > 0) {
        memmove(img + new_lines * WIDTH, img, (size_t)(strip_h - new_lines) * WIDTH * sizeof(uint32_t));
    }
    for (int32_t y = 0; y < new_lines; y++) {
        paint_line(row_at(top, y), y, center, span, lo, k);
    }
    if (new_lines) {
        paint_marks(0, new_lines, r0->width ? r0->pb_half : 0, span);
    }
    img_h = strip_h;
    img_span = span;
    img_center = center;
    img_written = n_written;
    copy_out(buf, portrait, stride, strip_h, 0, strip_h);

    if (span_label) {
        char s[16];

        lv_snprintf(s, sizeof(s), "%u kHz", (unsigned)(span / 1000));
        lv_label_set_text(span_label, s);
        lv_obj_set_pos(span_label, 6, strip_h - 18);    /* at the bottom: the band names come over the top */
        lv_obj_clear_flag(span_label, LV_OBJ_FLAG_HIDDEN);
    }
}

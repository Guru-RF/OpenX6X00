/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "smeter.h"
#include "tx_meters.h"
#include "ops_room.h"
#include "gui_styles.h"
#include "theme.h"
#include "gui_events.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "util.h"
#include "scheduler.h"

#include <math.h>
#include <stdio.h>

/* The reading in dB, S1 to S9+40, after PRE, ATT and the calibration */
static float            meter_db = S1;          /* smoothed: the bars and the needle */
static float            meter_db_raw = S1;      /* as read: the dB value, CAT, the app */
static _Atomic uint32_t  meter_seq_n;            /* readings so far (meter_seq) */
static float            noise_level = S_MIN;    /* the bars under it stay grey */

/* The peak bar stays PEAK_HOLD_MS, then each reading takes off PEAK_FALL dB for every second
 * past the hold: it falls slowly first, then faster and faster */
#define PEAK_HOLD_MS    1500
#define PEAK_FALL       20

static float            meter_peak = S1;
static int64_t          peak_ms;                /* when the peak was set */
static bool             in_tx;        /* TX: no reading from the receiver, framed red */
static bool             tx_readings;  /* TX with neither the TX panel nor the apps' strip: their readings here (#166) */

/* PRE and ATT shift the reading (by the settings cfg_smeter_pre / cfg_smeter_att, measured or
 * from Settings > TX Cal > S-METER > Measure; the X6200's attenuator figure is the band's) and the
 * noise floor: kept here, meter_update() and meter_set_noise() run in the DSP thread */
static bool             pre_on, att_on;
#define FLOOR_DB        14.0f

static lv_obj_t         *obj;
static lv_obj_t         *db_val_label;

/* The bars: 3 dB each up to S9, then 5 dB; a scale number every 4 bars */
#define BARS            25
#define S9_BAR          16

static const char *const scale_text[] = {"S1", "3", "5", "7", "9", "+20", "+40"};

static int16_t bar_db(int i) {
    return i <= S9_BAR ? S1 + 3 * i : S9 + 5 * (i - S9_BAR);
}

static void keep_bool(Subject *subj, void *user_data) {
    *(bool *)user_data = subject_i_get((SubjectInt *)subj);
}

static void show_value(void *unused) {
    if (in_tx) return;     /* TX: "TX" stays, or the value stays hidden and the TX readings are drawn */
    if (!theme_shape.vision) {
        lv_label_set_text_fmt(db_val_label, "%.1f", meter_db_raw);
    } else if (meter_db_raw < S9 + 1) {   /* 6 dB a unit */
        lv_label_set_text_fmt(db_val_label, "S%d", (int)LV_CLAMP(1, 1 + (meter_db_raw - S1 + 3) / 6, 9));
    } else {
        lv_label_set_text_fmt(db_val_label, "S9+%d", (int)(meter_db_raw - S9 + 0.5f));
    }
}

#define VISION_VALUE_W 150   /* VISION: room for "S9+40" right of the bars */

/* VISION keeps the bars: its meter is a wide, low row with the value beside it */
static bool needle_on(void) {
    return param_i_get(cfg_smeter_needle) && !theme_shape.vision && !theme_shape.deck && !theme_shape.mono &&
           !theme_shape.codec;
}

/* The dB value: under the bars, or under the dial in the needle's dim colour */
static void db_label_place(void) {
    if (theme_shape.codec) {        /* top right, beside SIGNAL */
        lv_obj_set_style_text_font(db_val_label, &sony_20, 0);
        lv_obj_set_style_text_color(db_val_label, TC(text_value), 0);
        lv_obj_align(db_val_label, LV_ALIGN_TOP_RIGHT, -12, 3);
    } else if (theme_shape.ops_room) {     /* drawn with the hexagons (ops_room.c) */
        lv_obj_add_flag(db_val_label, LV_OBJ_FLAG_HIDDEN);
    } else if (theme_shape.vision) {
        lv_obj_align(db_val_label, LV_ALIGN_RIGHT_MID, -12, 0);
    } else if (needle_on()) {
        lv_obj_set_style_text_font(db_val_label, &sony_20, 0);
        lv_obj_set_style_text_color(db_val_label, param_i_get(cfg_smeter_lamp) ? lv_color_hex(0x4a2e0e) : TC(text_dim), 0);
        lv_obj_align(db_val_label, LV_ALIGN_BOTTOM_MID, 0, -1);
    } else {
        lv_obj_set_style_text_font(db_val_label, &sony_20, 0);
        lv_obj_set_style_text_color(db_val_label, TC(text), 0);
        lv_obj_align(db_val_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    }
}

static void on_needle_change(Subject *subj, void *user_data) {
    db_label_place();
    lv_obj_invalidate(obj);
}

static float needle_u(float db);

/* S-meter colours (Settings): green, yellow, red of the theme along u = 0..1 of the scale */
static lv_color_t ramp(float u) {
    u = LV_CLAMP(0.0f, u, 1.0f);
    if (u < 0.5f)
        return lv_color_mix(TC(warn), TC(ok), (uint8_t)(u * 2 * 255));
    return lv_color_mix(TC(tx), TC(warn), (uint8_t)((u - 0.5f) * 2 * 255));
}

/* With the theme colours: the bars up to S9 and the needle scale in their own colour (Settings,
 * web Display page), else the theme's; after dl2zw's meter colours, OpenX6100 */
static lv_color_t bar_own(void) {
    int32_t c = param_i_get(cfg_smeter_bar_color);
    return c < 0 ? TC(meter_bar) : lv_color_hex(c);
}

/* Bar i of n at db */
static lv_color_t bar_color(int16_t db, uint8_t i, uint8_t n) {
    int32_t colors = param_i_get(cfg_smeter_color);

    if (colors)     /* every lit bar coloured, also under the noise level (else weak signals stay grey) */
        return ramp(needle_u(colors == 1 ? db : meter_db));   /* 1 scale, 2 level */
    if (db < S9 && param_i_get(cfg_smeter_bar_color) >= 0)    /* own colour, also under the noise level */
        return bar_own();
    if (theme_shape.meter_gradient) {
        return lv_color_mix(TC(meter_high), TC(meter_weak), i * 255 / (n - 1));
    }
    if (db >= S9) {
        /* Green at S9 fading to red at +40 */
        uint8_t mix = (db - S9) * 255 / (S9_40 - S9);
        return lv_color_mix(TC(meter_high), TC(meter_low), mix);
    }
    return db <= noise_level ? TC(text_mute) : TC(meter_bar);
}

/* Needle S-meter (Settings, Display): a dial whose pivot is under the box, so only the top of
 * the arc shows. S1..S9 take 70% of the arc in meter_bar, +20 and +40 the rest in meter_high
 * (thicker, the red zone of a real meter), the needle in tx: every colour from the theme. */
#define NEEDLE_A0       (-32)          /* degrees from vertical, left end */
#define NEEDLE_A1       32
#define NEEDLE_S9_U     0.7f

static float needle_u(float db) {
    float u = db <= S9 ? (db - S1) / (float)(S9 - S1) * NEEDLE_S9_U
                       : NEEDLE_S9_U + (db - S9) / (float)(S9_40 - S9) * (1.0f - NEEDLE_S9_U);
    return LV_CLAMP(0.0f, u, 1.0f);
}

static lv_point_t needle_pt(lv_point_t c, float a_deg, lv_coord_t r) {
    float      a = a_deg * (float)M_PI / 180.0f;
    lv_point_t p = {c.x + (lv_coord_t)lroundf(r * sinf(a)), c.y - (lv_coord_t)lroundf(r * cosf(a))};
    return p;
}

/* Needle lamp (Settings): an amber dial brighter low in the middle, as a bulb behind it. LVGL 8
 * has no radial gradient, so the dial is computed per pixel once per box size (main screen and
 * walkie) into an ARGB image: smooth glow, anti-aliased rounded corners, no banding */
#define LAMP_INK        lv_color_hex(0x1a1006)
#define LAMP_RED        lv_color_hex(0xb01e10)

typedef struct {
    lv_img_dsc_t dsc;
    lv_coord_t   radius;
} lamp_img_t;

static lamp_img_t lamp_cache[2];

static const lv_img_dsc_t *lamp_img(lv_coord_t w, lv_coord_t h, lv_coord_t radius) {
    lamp_img_t *c = NULL;

    for (int i = 0; i < 2; i++) {
        lamp_img_t *e = &lamp_cache[i];

        if (e->dsc.data && e->dsc.header.w == w && e->dsc.header.h == h && e->radius == radius)
            return &e->dsc;
        if (!c && !e->dsc.data)
            c = e;
    }
    if (!c) {                               /* both slots taken by other sizes: reuse the first */
        c = &lamp_cache[0];
        lv_img_cache_invalidate_src(&c->dsc);   /* LVGL caches by descriptor: drop the stale entry */
        lv_mem_free((void *)c->dsc.data);
        c->dsc.data = NULL;
    }

    lv_color32_t *px = lv_mem_alloc((size_t)w * h * sizeof(lv_color32_t));

    if (!px)
        return NULL;

    const float lo[3] = {0x9a, 0x5e, 0x1c}, hi[3] = {0xe0, 0xa0, 0x48}, bulb[3] = {0xff, 0xe2, 0xa8};
    float       cx = (w - 1) / 2.0f, rad = LV_MIN(radius, LV_MIN(w, h) / 2);

    for (lv_coord_t y = 0; y < h; y++) {
        float t = (float)y / (h - 1);

        for (lv_coord_t x = 0; x < w; x++) {
            /* bulb just below the bottom centre, elliptical (wider than tall) */
            float dx = (x - cx) / (h * 1.9f), dy = (h * 1.05f - y) / (h * 1.25f);
            float g  = 0.62f * expf(-(dx * dx + dy * dy) * 1.6f);
            float ex = fabsf(x - cx) / cx;
            float k  = 0.10f * ex * ex;     /* edge darkening, as the light falls off */

            /* anti-aliased rounded corners */
            float qx = LV_MAX(rad - x - 0.5f, x + 0.5f - (w - rad)), qy = LV_MAX(rad - y - 0.5f, y + 0.5f - (h - rad));
            float a  = 1.0f;

            if (qx > 0 && qy > 0)
                a = LV_CLAMP(0.0f, rad + 0.5f - sqrtf(qx * qx + qy * qy), 1.0f);

            uint8_t ch[3];

            for (int i = 0; i < 3; i++) {
                float v = lo[i] + (hi[i] - lo[i]) * t;

                v = v + (bulb[i] - v) * g;
                v *= 1.0f - k;
                ch[i] = (uint8_t)LV_CLAMP(0, (int)(v + 0.5f), 255);
            }
            lv_color32_t *o = &px[y * w + x];

            o->ch.red   = ch[0];
            o->ch.green = ch[1];
            o->ch.blue  = ch[2];
            o->ch.alpha = (uint8_t)(a * 255.0f + 0.5f);
        }
    }

    c->dsc.header.always_zero = 0;
    c->dsc.header.cf          = LV_IMG_CF_TRUE_COLOR_ALPHA;
    c->dsc.header.w           = w;
    c->dsc.header.h           = h;
    c->dsc.data_size          = (uint32_t)w * h * sizeof(lv_color32_t);
    c->dsc.data               = (const uint8_t *)px;
    c->radius                 = radius;
    return &c->dsc;
}

static void lamp_draw(lv_obj_t *obj, lv_draw_ctx_t *draw_ctx) {
    const lv_img_dsc_t *img = lamp_img(lv_obj_get_width(obj), lv_obj_get_height(obj),
                                       lv_obj_get_style_radius(obj, LV_PART_MAIN));

    if (img) {
        lv_draw_img_dsc_t dsc;

        lv_draw_img_dsc_init(&dsc);
        lv_draw_img(draw_ctx, &dsc, &obj->coords, img);
    }
}

static void bars_draw_mono(lv_obj_t *o, lv_draw_ctx_t *draw_ctx);

void meter_needle_draw(lv_obj_t *obj, lv_draw_ctx_t *draw_ctx) {
    if (theme_shape.mono) {     /* the walkie's meter too: no needle in 1bit */
        bars_draw_mono(obj, draw_ctx);
        return;
    }

    lv_coord_t          w = lv_obj_get_width(obj);
    lv_coord_t          big = lv_obj_get_height(obj) > 120 ? 8 : 0;    /* the walkie screen: bigger labels, rounder arc */
    lv_coord_t          r = w * (big ? 62 : 64) / 100;     /* wide and flat: 166 px on the 260 px box */
    lv_point_t          c = {obj->coords.x1 + w / 2, obj->coords.y1 + (big ? 48 : 28) + r};   /* the arc top 28 px down */
    lv_draw_arc_dsc_t   arc_dsc;
    lv_draw_line_dsc_t  line_dsc;
    lv_draw_label_dsc_t label_dsc;
    float               span = NEEDLE_A1 - NEEDLE_A0;

    bool                lamp    = param_i_get(cfg_smeter_lamp);         /* amber dial, dark ink, red zone */
    int32_t             colors  = lamp ? 0 : param_i_get(cfg_smeter_color);
    bool                colored = colors != 0;      /* the needle in text; 1 the arc green to red, 2 lit to the level */
    float               level_u = needle_u(meter_db);
    lv_color_t          ink     = lamp ? LAMP_INK : TC(text);
    lv_color_t          scale   = lamp ? LAMP_INK : bar_own();
    lv_color_t          high    = lamp ? LAMP_RED : TC(meter_high);
    lv_color_t          over    = lamp ? LAMP_RED : TC(meter_over);

    if (lamp)
        lamp_draw(obj, draw_ctx);

    /* LVGL arcs: 0 degrees at 3 o'clock, clockwise; vertical up is 270 */
    lv_draw_arc_dsc_init(&arc_dsc);
    if (colored) {
        for (int k = 0; k < (int)span; k++) {      /* a degree a segment, each in its colour */
            float u = (k + 0.5f) / span;

            arc_dsc.color = colors == 1 ? ramp(u) : u <= level_u ? ramp(level_u) : TC(text_mute);
            arc_dsc.width = u < NEEDLE_S9_U ? 3 : 5;
            lv_draw_arc(draw_ctx, &arc_dsc, &c, r + (u < NEEDLE_S9_U ? 0 : 2), 270 + NEEDLE_A0 + k, 270 + NEEDLE_A0 + k + 1);
        }
    } else {
        arc_dsc.color = scale;
        arc_dsc.width = 2;
        lv_draw_arc(draw_ctx, &arc_dsc, &c, r, 270 + NEEDLE_A0, 270 + NEEDLE_A0 + (int)(span * NEEDLE_S9_U));
        arc_dsc.color = high;
        arc_dsc.width = 5;
        lv_draw_arc(draw_ctx, &arc_dsc, &c, r + 2, 270 + NEEDLE_A0 + (int)(span * NEEDLE_S9_U), 270 + NEEDLE_A1);
    }

    lv_draw_line_dsc_init(&line_dsc);
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.font = big ? &sony_32 : &sony_22;
    lv_coord_t lab = big ? 15 : 13;    /* the numbers off the arc */

    for (int s = 1; s <= 9; s++) {      /* a tick a S unit, long ones and numbers on the odd */
        float      a   = NEEDLE_A0 + span * needle_u(S1 + (s - 1) * 6);
        bool       big = s % 2;
        lv_point_t p1  = needle_pt(c, a, r), p2 = needle_pt(c, a, r - (big ? 9 : 5));

        line_dsc.color = colors == 1 ? ramp(needle_u(S1 + (s - 1) * 6)) : scale;
        line_dsc.width = big ? 2 : 1;
        lv_draw_line(draw_ctx, &line_dsc, &p1, &p2);
        if (big) {
            char       txt[4];
            lv_point_t sz, l = needle_pt(c, a, r + lab);
            lv_area_t  area;

            snprintf(txt, sizeof(txt), s == 1 ? "S1" : "%d", s);
            lv_txt_get_size(&sz, txt, label_dsc.font, 0, 0, LV_COORD_MAX, 0);
            area.x1 = LV_CLAMP(obj->coords.x1 + 2, l.x - sz.x / 2, obj->coords.x2 - 2 - sz.x);   /* S1, +40 inside the box */
            area.y1 = l.y - sz.y / 2;
            area.x2 = area.x1 + sz.x;
            area.y2 = area.y1 + sz.y;
            label_dsc.color = ink;
            lv_draw_label(draw_ctx, &label_dsc, &area, txt, NULL);
        }
    }
    for (int d = 10; d <= 40; d += 10) {
        float      a   = NEEDLE_A0 + span * needle_u(S9 + d);
        bool       big = d % 20 == 0;
        lv_point_t p1  = needle_pt(c, a, r + 2), p2 = needle_pt(c, a, r - (big ? 10 : 6));

        line_dsc.color = colors == 1 ? ramp(needle_u(S9 + d)) : high;
        line_dsc.width = big ? 2 : 1;
        lv_draw_line(draw_ctx, &line_dsc, &p1, &p2);
        if (big) {
            char       txt[4];
            lv_point_t sz, l = needle_pt(c, a, r + lab + 1);
            lv_area_t  area;

            snprintf(txt, sizeof(txt), "+%d", d);
            lv_txt_get_size(&sz, txt, label_dsc.font, 0, 0, LV_COORD_MAX, 0);
            area.x1 = LV_CLAMP(obj->coords.x1 + 2, l.x - sz.x / 2, obj->coords.x2 - 2 - sz.x);   /* S1, +40 inside the box */
            area.y1 = l.y - sz.y / 2;
            area.x2 = area.x1 + sz.x;
            area.y2 = area.y1 + sz.y;
            label_dsc.color = over;
            lv_draw_label(draw_ctx, &label_dsc, &area, txt, NULL);
        }
    }

    /* The needle, from the pivot to just past the arc */
    lv_point_t tip = needle_pt(c, NEEDLE_A0 + span * needle_u(meter_db), r + 2);

    line_dsc.color       = lamp ? LAMP_RED : TC(tx);   /* red, as on a real meter */
    line_dsc.width       = 4;
    line_dsc.round_end   = 1;
    lv_draw_line(draw_ctx, &line_dsc, &c, &tip);
}

/* TX with the TX panel or the apps' strip up: the whole box veiled red */
static void tx_veil(lv_obj_t *o, lv_draw_ctx_t *draw_ctx) {
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = TC(err);
    dsc.bg_opa   = LV_OPA_30;
    dsc.radius   = lv_obj_get_style_radius(o, LV_PART_MAIN);
    lv_draw_rect(draw_ctx, &dsc, &o->coords);
}

/* Deck: S, SWR and ALC scales stacked, a thin segmented bar under the S numbers; with the TX readings in the box
 * the S row is PO, 0 to 10 W, and the three rows have their bars (#166) */
static void label_at(lv_draw_ctx_t *draw_ctx, lv_draw_label_dsc_t *dsc, lv_coord_t cx, lv_coord_t y, const char *txt,
                     bool left) {
    lv_point_t sz;
    lv_area_t  a;

    lv_txt_get_size(&sz, txt, dsc->font, 0, 0, LV_COORD_MAX, 0);
    a.x1 = left ? cx : cx - sz.x / 2;
    a.y1 = y;
    a.x2 = a.x1 + sz.x;
    a.y2 = y + sz.y;
    lv_draw_label(draw_ctx, dsc, &a, txt, NULL);
}

static lv_coord_t deck_x(float db) {   /* S1 at 58, S9 at 154, +40 at 233 */
    db = LV_CLAMP(S1, db, S9_40);
    return db <= S9 ? 58 + (db - S1) * 2 : 154 + (db - S9) * 79 / (S9_40 - S9);
}

/* Deck, the TX readings in the box: segments as the S row's under each scale, lit up to the reading: the power (0
 * at 58, 10 W at 233), the SWR (1 at 58, 5 at 258; yellow from 2, red from 3, below 2 in the web Display page's SWR
 * colour when one is set) and the ALC (0 at 60, 10 at 260) */
static void deck_tx_bars(lv_obj_t *obj, lv_draw_ctx_t *draw_ctx) {
    float              pwr, swr, alc;
    bool               alc_grey;
    int32_t            swr_own = param_i_get(cfg_swr_color);
    lv_draw_rect_dsc_t rect;

    tx_info_get(&pwr, &swr);
    alc = tx_info_get_alc(&alc_grey);

    const struct {
        lv_coord_t y, lit;
        int        n;
    } row[] = {
        {27, 58 + LV_CLAMP(0.0f, pwr, 10.0f) * 17.5f, 31},
        {57, 58 + (LV_CLAMP(1.0f, swr, 5.0f) - 1.0f) * 50.0f, 35},
        {87, 60 + LV_CLAMP(0.0f, alc, 10.0f) * 20.0f, 35},
    };

    lv_draw_rect_dsc_init(&rect);
    for (int r = 0; r < 3; r++) {
        lv_area_t a = {.y1 = obj->coords.y1 + row[r].y, .y2 = obj->coords.y1 + row[r].y + 6};

        for (int i = 0; i < row[r].n; i++) {
            lv_coord_t x = 66 + i * 11 / 2;

            a.x1 = obj->coords.x1 + x;
            a.x2 = a.x1 + 3;
            rect.bg_color = x > row[r].lit             ? TC(track)
                          : r == 1 && x >= 158         ? TC(bar_high)
                          : r == 1 && x >= 108         ? TC(bar_mid)
                          : r == 1 && swr_own >= 0     ? lv_color_hex(swr_own)
                          : r == 2 && alc_grey         ? TC(text_mute)
                                                       : TC(meter_bar);
            lv_draw_rect(draw_ctx, &rect, &a);
        }
    }
}

static void deck_draw(lv_obj_t *obj, lv_draw_ctx_t *draw_ctx) {
    static const char *const s_num[] = {"1", "3", "5", "7", "9"}, *const swr[] = {"1", "2", "3", "4", ">5"};
    lv_draw_label_dsc_t      dsc;
    lv_draw_rect_dsc_t       rect;
    lv_coord_t               x0 = obj->coords.x1, y0 = obj->coords.y1;
    char                     txt[4];
    bool                     alc_grey = false;

    if (tx_readings) tx_info_get_alc(&alc_grey);
    lv_draw_label_dsc_init(&dsc);
    dsc.font  = &sony_20;
    dsc.color = TC(text);
    if (tx_readings) {
        label_at(draw_ctx, &dsc, x0 + 14, y0 + 17, "PO", true);
        for (int i = 0; i <= 5; i++) {
            snprintf(txt, sizeof(txt), "%d", i * 2);
            label_at(draw_ctx, &dsc, x0 + 58 + i * 35, y0 + 4, txt, false);
        }
    } else {
        label_at(draw_ctx, &dsc, x0 + 14, y0 + 17, "S", true);
        for (int i = 0; i < 5; i++) label_at(draw_ctx, &dsc, x0 + 58 + i * 24, y0 + 4, s_num[i], false);
        dsc.color = TC(meter_over);
        label_at(draw_ctx, &dsc, x0 + 193, y0 + 4, "+20", false);
        label_at(draw_ctx, &dsc, x0 + 233, y0 + 4, "+40", false);
    }
    dsc.color = tx_readings ? TC(text) : TC(text_mute);   /* SWR and ALC lit with the TX readings, ALC grey in CW */
    label_at(draw_ctx, &dsc, x0 + 14, y0 + 47, "SWR", true);
    for (int i = 0; i < 5; i++) label_at(draw_ctx, &dsc, x0 + 58 + i * 50, y0 + 35, swr[i], false);
    if (alc_grey) dsc.color = TC(text_mute);
    label_at(draw_ctx, &dsc, x0 + 14, y0 + 77, "ALC", true);
    for (int i = 0; i <= 5; i++) {
        snprintf(txt, sizeof(txt), "%d", i * 2);
        label_at(draw_ctx, &dsc, x0 + 60 + i * 40, y0 + 65, txt, false);
    }
    if (tx_readings) {
        deck_tx_bars(obj, draw_ctx);
        return;
    }

    /* 31 segments to +40: lit to the level, the peak bright */
    lv_coord_t lvl = deck_x(meter_db), peak = deck_x(meter_peak);
    lv_area_t  a = {.y1 = y0 + 27, .y2 = y0 + 33};

    lv_draw_rect_dsc_init(&rect);
    for (int i = 0; i < 31; i++) {
        lv_coord_t x = 66 + i * 11 / 2;

        a.x1 = x0 + x;
        a.x2 = a.x1 + 3;
        rect.bg_color = x > lvl ? (x <= peak && x + 6 > peak ? TC(text) : TC(track))
                                : x >= 154 ? TC(meter_high) : TC(meter_bar);
        lv_draw_rect(draw_ctx, &rect, &a);
    }
}

/* R1CBU's bars layout fitted to the box: thin bars over the whole width, the scale under them
 * (VISION: the bars on top, the scale below, the value right of them), the dB value at the bottom */
static void bars_draw(lv_obj_t *o, lv_draw_ctx_t *draw_ctx) {
    lv_coord_t bars_w = lv_obj_get_width(o) - (theme_shape.vision ? VISION_VALUE_W : 0);
    uint8_t    step = (bars_w - 16 - 22) / (BARS - 1);   /* room for "+40" at the right */
    uint8_t    gap = step * 45 / 100;                     /* thin bars, wide gaps */
    lv_coord_t x0 = o->coords.x1 + 16;                    /* the centre of the first bar */
    lv_coord_t top = o->coords.y1;

    lv_draw_rect_dsc_t bar;
    lv_area_t          a;
    int                peak = 0;

    lv_draw_rect_dsc_init(&bar);
    bar.bg_opa = LV_OPA_80;
    a.y1 = top + (theme_shape.vision ? 4 : 6);
    a.y2 = top + (theme_shape.vision ? 20 : 43);
    for (int i = 0; i < BARS; i++) {
        if (meter_peak >= bar_db(i) - 1.5f) peak = i;
        if (meter_db < bar_db(i) - 1.5f) continue;
        a.x1 = x0 + i * step - step / 2 + gap / 2;
        a.x2 = a.x1 + step - gap;
        bar.bg_color = bar_color(bar_db(i), i, BARS);
        lv_draw_rect(draw_ctx, &bar, &a);
    }

    /* The peak above the level, bright: its bar colour from S9 (and with a gradient or the
     * S-meter colours), white below */
    if (peak > 0 && meter_db < bar_db(peak) - 1.5f) {
        bool coloured = bar_db(peak) >= S9 || theme_shape.meter_gradient || param_i_get(cfg_smeter_color);

        a.x1 = x0 + peak * step - step / 2 + gap / 2;
        a.x2 = a.x1 + step - gap;
        bar.bg_opa = LV_OPA_COVER;
        bar.bg_color = coloured ? bar_color(bar_db(peak), peak, BARS) : TC(text);
        lv_draw_rect(draw_ctx, &bar, &a);
    }

    lv_draw_label_dsc_t scale;
    lv_point_t          sz;

    lv_draw_label_dsc_init(&scale);
    scale.font = &sony_22;
    a.y1 = top + (theme_shape.vision ? 21 : 16);
    a.y2 = a.y1 + 18;
    for (int k = 0; k < (int)(sizeof(scale_text) / sizeof(scale_text[0])); k++) {
        lv_txt_get_size(&sz, scale_text[k], scale.font, 0, 0, LV_COORD_MAX, 0);
        a.x1 = x0 + step * 4 * k - sz.x / 2;
        a.x2 = a.x1 + sz.x;
        scale.color = k > 4 ? TC(meter_over) : TC(text);   /* +20, +40 */
        lv_draw_label(draw_ctx, &scale, &a, scale_text[k], NULL);
    }
}

/* 1bit: the bars as an LCD's segments, under the scale (which no longer runs over them). Lit above
 * the noise level: solid; lit at or under it (grey in colour): hollow; not lit: a tick on the bottom
 * row, a ruler to read the scale by; from S9 up 3 px taller, the red zone in one colour; the peak
 * solid. No lamp, no own colours */
static void bars_draw_mono(lv_obj_t *o, lv_draw_ctx_t *draw_ctx) {
    uint8_t             step = (lv_obj_get_width(o) - 16 - 22) / (BARS - 1);
    uint8_t             gap = step * 45 / 100;
    lv_coord_t          x0 = o->coords.x1 + 16, top = o->coords.y1;
    lv_draw_rect_dsc_t  seg;
    lv_draw_label_dsc_t scale;
    lv_area_t           a;
    lv_point_t          sz;
    int                 peak = 0;

    for (int i = 0; i < BARS; i++) {
        if (meter_peak >= bar_db(i) - 1.5f) peak = i;
    }
    lv_draw_rect_dsc_init(&seg);
    seg.bg_color = seg.border_color = TC(text);
    for (int i = 0; i < BARS; i++) {
        bool lit = meter_db >= bar_db(i) - 1.5f;

        a.x1 = x0 + i * step - step / 2 + gap / 2;
        a.x2 = a.x1 + step - gap;
        a.y1 = top + (i > S9_BAR ? 19 : 22);
        a.y2 = top + 37;
        if (!lit && !(i == peak && peak > 0)) {
            a.y1 = a.y2;                                    /* the ruler */
            seg.bg_opa = LV_OPA_COVER;
            seg.border_width = 0;
        } else if (lit && i != peak && bar_db(i) <= noise_level) {
            seg.bg_opa = LV_OPA_TRANSP;                     /* under the noise: hollow */
            seg.border_width = 1;
        } else {
            seg.bg_opa = LV_OPA_COVER;
            seg.border_width = 0;
        }
        lv_draw_rect(draw_ctx, &seg, &a);
    }

    lv_draw_label_dsc_init(&scale);
    scale.font = &sony_22;
    scale.color = TC(text);
    a.y1 = top + 1;
    a.y2 = a.y1 + 18;
    for (int k = 0; k < (int)(sizeof(scale_text) / sizeof(scale_text[0])); k++) {
        lv_txt_get_size(&sz, scale_text[k], scale.font, 0, 0, LV_COORD_MAX, 0);
        a.x1 = x0 + step * 4 * k - sz.x / 2;
        a.x2 = a.x1 + sz.x;
        lv_draw_label(draw_ctx, &scale, &a, scale_text[k], NULL);
    }
}

/* Codec: SIGNAL over a framed bar of segments, red above S9, the peak bright, the scale under it */
lv_coord_t meter_x70(float db, lv_coord_t w) {
    db = LV_CLAMP(S1, db, S9_40);
    return db <= S9 ? (db - S1) * w * 7 / 10 / (S9 - S1) : w * 7 / 10 + (db - S9) * w * 3 / 10 / (S9_40 - S9);
}

static void signal_draw(lv_obj_t *o, lv_draw_ctx_t *draw_ctx) {
    static const struct { int16_t db; const char *t; } marks[] = {
        {S1, "1"}, {S3, "3"}, {S5, "5"}, {S7, "7"}, {S9, "9"}, {S9_20, "+20"}, {S9_40, "+40"}};
    lv_coord_t          x0 = o->coords.x1 + 12, x1 = o->coords.x2 - 12, w = x1 - x0, y0 = o->coords.y1;
    lv_draw_label_dsc_t dsc;
    lv_draw_rect_dsc_t  rect;
    lv_area_t           a;

    lv_draw_label_dsc_init(&dsc);
    dsc.font = &sony_20;
    dsc.color = TC(text_head);
    label_at(draw_ctx, &dsc, x0, y0 + 3, "SIGNAL", true);

    /* the frame */
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_TRANSP;
    rect.border_color = TC(meter_weak);
    rect.border_width = 1;
    a = (lv_area_t){x0, y0 + 24, x1, y0 + 41};
    lv_draw_rect(draw_ctx, &rect, &a);

    /* segments 3 px wide, 1 px apart, lit to the level */
    lv_coord_t lvl = in_tx ? 0 : meter_x70(meter_db, w - 4), peak = in_tx ? 0 : meter_x70(meter_peak, w - 4);
    lv_coord_t s9 = meter_x70(S9, w - 4);

    lv_draw_rect_dsc_init(&rect);
    for (lv_coord_t x = 0; x + 3 <= w - 4; x += 4) {
        bool lit = x < lvl, top = !lit && x <= peak && x + 4 > peak;

        rect.bg_color = top ? TC(text_value) : !lit ? TC(track) : x >= s9 ? TC(meter_over) : TC(meter_bar);
        a = (lv_area_t){x0 + 2 + x, y0 + 26, x0 + 2 + x + 2, y0 + 39};
        lv_draw_rect(draw_ctx, &rect, &a);
    }

    /* the scale */
    dsc.font = &sony_14;
    for (int i = 0; i < (int)(sizeof(marks) / sizeof(marks[0])); i++) {
        dsc.color = marks[i].db > S9 ? TC(meter_over) : TC(text_dim);
        label_at(draw_ctx, &dsc, x0 + 2 + meter_x70(marks[i].db, w - 4), y0 + 43, marks[i].t, false);
    }
}

static void meter_draw_cb(lv_event_t *e) {
    lv_obj_t      *o = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);

    if (theme_shape.ops_room && !tx_readings) {      /* the TX readings as in any theme */
        ops_room_meter_draw(o, draw_ctx, in_tx ? S1 : meter_db, in_tx ? S1 : meter_peak, meter_db_raw, in_tx);
        if (in_tx) tx_veil(o, draw_ctx);
        return;
    }
    if (theme_shape.deck) {
        deck_draw(o, draw_ctx);
        return;
    }
    if (theme_shape.codec && !tx_readings) {     /* the TX readings as in any theme */
        signal_draw(o, draw_ctx);
        if (in_tx) tx_veil(o, draw_ctx);
        return;
    }
    if (tx_readings) tx_meters_draw(o, draw_ctx);
    else if (theme_shape.mono) bars_draw_mono(o, draw_ctx);
    else if (needle_on()) meter_needle_draw(o, draw_ctx);
    else bars_draw(o, draw_ctx);
    if (in_tx && !tx_readings) tx_veil(o, draw_ctx);
}

/* TX: the receiver gives no reading; the box framed red. With the TX panel or the apps' strip up (tx_info_shown(),
 * they show the readings big) the reading at rest and "TX" in red in place of the value, as up to v0.3.8; with
 * neither, the box shows the power, SWR and ALC, its value hidden (#166).
 * RX: the value in its own colour again, the frame of the style, blank until the next reading: with no data from
 * the base (#152, a tune given up for it) "TX" stayed on the meter in receive */
static void radio_cb(lv_event_t *e) {
    in_tx = lv_event_get_code(e) == EVENT_RADIO_TX;
    tx_readings = in_tx && !tx_info_shown();
    if (in_tx) {
        meter_db = meter_db_raw = meter_peak = S1;
        if (tx_readings) {
            lv_obj_add_flag(db_val_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(db_val_label, "TX");
            lv_obj_set_style_text_color(db_val_label, TC(err), 0);
        }
        lv_obj_set_style_border_color(obj, TC(err), 0);
        lv_obj_set_style_border_width(obj, 5, 0);
    } else {
        lv_label_set_text(db_val_label, "");
        lv_obj_set_style_text_color(db_val_label, TC(text), 0);
        db_label_place();
        if (!theme_shape.deck && !theme_shape.ops_room) lv_obj_clear_flag(db_val_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_local_style_prop(obj, LV_STYLE_BORDER_COLOR, 0);
        lv_obj_remove_local_style_prop(obj, LV_STYLE_BORDER_WIDTH, 0);
    }
    lv_obj_invalidate(obj);
}

void meter_tx_redraw(void) {
    if (tx_readings && obj) lv_obj_invalidate(obj);
}

lv_obj_t *meter_init(lv_obj_t *parent) {
    obj = lv_obj_create(parent);
    lv_obj_add_style(obj, &meter_style, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(obj, radio_cb, EVENT_RADIO_TX, NULL);
    lv_obj_add_event_cb(obj, radio_cb, EVENT_RADIO_RX, NULL);
    lv_obj_add_event_cb(obj, meter_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    subject_subscribe_delayed_and_notify((Subject *)cfg_cur_pre, keep_bool, &pre_on);
    subject_subscribe_delayed_and_notify((Subject *)cfg_cur_att, keep_bool, &att_on);

    /* The value: under the bars; VISION big at the right; deck none (the scales fill the box) */
    db_val_label = lv_label_create(obj);
    if (theme_shape.deck) lv_obj_add_flag(db_val_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(db_val_label, theme_shape.vision ? &sony_40 : &sony_20, 0);
    lv_obj_set_style_text_color(db_val_label, TC(text), 0);
    db_label_place();
    lv_label_set_text(db_val_label, "");

    /* The S-meter settings change the drawing live */
    subject_subscribe_delayed((Subject *)cfg_smeter_needle, on_needle_change, NULL);
    subject_subscribe_delayed((Subject *)cfg_smeter_color, on_needle_change, NULL);
    subject_subscribe_delayed((Subject *)cfg_smeter_lamp, on_needle_change, NULL);
    subject_subscribe_delayed((Subject *)cfg_smeter_bar_color, on_needle_change, NULL);
    return obj;
}

/* Calibration of the current band (Settings, S-meter cal: the baseband reads a different level
 * on each band) and RF gain, which lowers the reading cfg_smeter_rfg hundredths of a dB a step
 * (0.60 on one radio, 3 broadcast carriers, 100 to 0): the reading is the same at any RF gain,
 * as PRE and ATT */
static float meter_cal_db(void) {
    return param_i_get(cfg_smeter_cal) + 0.01f * param_i_get(cfg_smeter_rfg) * (100 - param_i_get(cfg_rfgain));
}

void meter_set_noise(float val) {
    float level = val + meter_cal_db();   /* on the same scale as the reading it greys out */

#ifdef BOARD_X6200
    /* By the reading's own corrections: the X6200's attenuator takes 24 to about 62 dB off (#125), and with
     * FLOOR_DB the noise read with ATT on showed as a signal, 15 to 37 dB over the grey */
    if (att_on) level += param_i_get(cfg_smeter_att);
    if (pre_on) level -= param_i_get(cfg_smeter_pre);
#else
    if (att_on) level += FLOOR_DB;
    if (pre_on) level -= FLOOR_DB;
#endif
    noise_level = level;
}

void meter_update(float db, float beta) {
    /* A NaN (empty PSD while the span changes) would stick in the smoothing forever */
    if (isnan(db) || in_tx) return;
    if (!isfinite(meter_db) || !isfinite(meter_peak)) meter_db = meter_peak = S1;

    if (att_on) db += param_i_get(cfg_smeter_att);
    if (pre_on) db -= param_i_get(cfg_smeter_pre);
    db = LV_CLAMP(S1, db + meter_cal_db(), S9_40);

    int64_t ms = get_time();

    if (db > meter_peak) {
        meter_peak = db;
        peak_ms = ms;
    } else if (ms - peak_ms > PEAK_HOLD_MS) {
        meter_peak -= (ms - peak_ms - PEAK_HOLD_MS) * PEAK_FALL / 1000;
    }
    meter_db_raw = db;
    meter_seq_n++;
    meter_db = meter_db * beta + db * (1.0f - beta);
    scheduler_put_coalesced(show_value);
    event_send(obj, LV_EVENT_REFRESH, NULL);
}

int16_t meter_get_raw_db(void) {
    return meter_db_raw;
}

float meter_get_db(void) {
    return meter_db_raw;
}

uint32_t meter_seq(void) {
    return meter_seq_n;
}

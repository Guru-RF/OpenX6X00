/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "band_plan.h"

#include "cfg/cfg_api.h"
#include "styles.h"
#include "theme.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "fonts/font_select.h"

/* VISION: the sizes of the other themes here (an overlay on VISION's shorter waterfall) */
#define NF(n) (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS) : &sony_##n)

#define WIDTH   800
#define POOL    16      /* segments in view: a span is 100 kHz at most */
#define MAX_SEG 96      /* the IARU table and the bands added to the band list */
#define BANDS   48
#define OPA     LV_OPA_80     /* the waterfall shows through a little, the labels stay readable */

extern lv_font_t sony_18;


typedef struct {
    int32_t   start, end;   /* Hz */
    bp_type_t type;
} bp_seg_t;

/* IARU Region 1, adjacent ranges of the same type merged */
static const bp_seg_t plan[] = {
    {  1810000,  1838000, BP_CW  }, {  1838000,  1843000, BP_DIG }, {  1843000,  2000000, BP_ALL },
    {  3500000,  3570000, BP_CW  }, {  3570000,  3600000, BP_DIG }, {  3600000,  3800000, BP_ALL },
    {  5351500,  5354000, BP_CW  }, {  5354000,  5366000, BP_ALL }, {  5366000,  5366500, BP_DIG },  /* WRC-15 */
    {  7000000,  7040000, BP_CW  }, {  7040000,  7050000, BP_DIG }, {  7050000,  7200000, BP_ALL },
    { 10100000, 10130000, BP_CW  }, { 10130000, 10150000, BP_DIG },
    { 14000000, 14070000, BP_CW  }, { 14070000, 14099000, BP_DIG }, { 14099000, 14101000, BP_BCN },
    { 14101000, 14112000, BP_DIG }, { 14112000, 14350000, BP_ALL },
    { 18068000, 18095000, BP_CW  }, { 18095000, 18109000, BP_DIG }, { 18109000, 18111000, BP_BCN },
    { 18111000, 18120000, BP_DIG }, { 18120000, 18168000, BP_ALL },
    { 21000000, 21070000, BP_CW  }, { 21070000, 21120000, BP_DIG }, { 21120000, 21149000, BP_CW  },
    { 21149000, 21151000, BP_BCN }, { 21151000, 21450000, BP_ALL },
    { 24890000, 24915000, BP_CW  }, { 24915000, 24929000, BP_DIG }, { 24929000, 24931000, BP_BCN },
    { 24931000, 24990000, BP_ALL },
    { 28000000, 28070000, BP_CW  }, { 28070000, 28190000, BP_DIG }, { 28190000, 28225000, BP_BCN },
    { 28225000, 29700000, BP_ALL },
    { 50000000, 50100000, BP_CW  }, { 50100000, 50300000, BP_ALL }, { 50300000, 50400000, BP_DIG },
    { 50400000, 50500000, BP_BCN }, { 50500000, 52000000, BP_ALL },
};

static const theme_color_t colors[] = { TC_bp_cw, TC_bp_dig, TC_bp_ssb, TC_bp_bcn };
/* Longest first: the first label that fits the segment is used */
static const char *const labels[][2] = {
    { "CW", NULL }, { "DIG", NULL }, { "ALL MODES / SSB", "SSB" }, { "BCN", NULL },
};

typedef struct {
    lv_obj_t *box;
    lv_obj_t *label;
} seg_t;

static seg_t   segs[POOL];
static bool    ready;
static int32_t span_center, span_half;

/* The plan in use: plan[] and, where it has nothing, the active bands of the band list (a band added on the web
 * page, "60m CW" at 5.3 MHz, had no strip and no CW keyboard: issue #5). Read from any thread (remote_server) */
static bp_seg_t        cur[MAX_SEG];
static int             ncur;
static pthread_mutex_t cur_lock = PTHREAD_MUTEX_INITIALIZER;

/* A band's own segment type, from its name as the presets are named ("40m CW", "20m SSB"); its last used mode
 * is no type: it changes with every mode change */
static bp_type_t type_from_name(const char *name) {
    if (strcasestr(name, "CW")) return BP_CW;
    if (strcasestr(name, "DIG") || strcasestr(name, "FT8") || strcasestr(name, "FT4") || strcasestr(name, "DATA")) return BP_DIG;
    if (strcasestr(name, "BCN") || strcasestr(name, "BEACON")) return BP_BCN;
    return BP_ALL;
}

static int by_start(const void *a, const void *b) {
    const bp_seg_t *x = a, *y = b;

    return x->start < y->start ? -1 : x->start > y->start;
}

void band_plan_reload(void) {
    static int32_t ids[BANDS], start[BANDS], stop[BANDS];
    static char    names[BANDS][16];
    static bp_seg_t list[MAX_SEG];
    int            nb = cfg_bands_active_ids(ids, start, stop, names, BANDS);
    int            n  = sizeof(plan) / sizeof(plan[0]);

    memcpy(list, plan, sizeof(plan));
    for (int i = 0; i < nb; i++) {
        /* Broadcast and CB (ids 100-299, sql/params.sql) are no amateur bands */
        if (ids[i] >= 100 && ids[i] < 300) {
            continue;
        }
        int32_t   a = start[i], e = stop[i];
        bp_type_t t = type_from_name(names[i]);
        int       base = n;

        qsort(list, n, sizeof(list[0]), by_start);
        /* The parts of [a, e] no segment covers yet */
        for (int k = 0; k < base && a < e && n < MAX_SEG; k++) {
            if (list[k].end <= a) continue;
            if (list[k].start >= e) break;
            if (list[k].start > a) list[n++] = (bp_seg_t){a, list[k].start, t};
            a = list[k].end > a ? list[k].end : a;
        }
        if (a < e && n < MAX_SEG) list[n++] = (bp_seg_t){a, e, t};
    }
    qsort(list, n, sizeof(list[0]), by_start);
    pthread_mutex_lock(&cur_lock);
    memcpy(cur, list, n * sizeof(list[0]));
    ncur = n;
    pthread_mutex_unlock(&cur_lock);
}

static void redraw(void) {
    int n = 0;

    if (!ready) {
        return;
    }
    if (span_half > 0 && param_i_get(cfg_band_plan)) {
        int32_t left = span_center - span_half;
        int64_t span = 2 * (int64_t)span_half;

        pthread_mutex_lock(&cur_lock);
        for (int i = 0; i < ncur && n < POOL; i++) {
            const bp_seg_t *p    = &cur[i];
            int64_t         a    = (int64_t)(p->start - left) * WIDTH / span;
            int64_t         b    = (int64_t)(p->end - left) * WIDTH / span;
            seg_t          *s    = &segs[n];
            const char     *text = NULL;

            if (b <= 0 || a >= WIDTH) {
                continue;
            }
            if (a < 0)     a = 0;
            if (b > WIDTH) b = WIDTH;
            if (b <= a)    b = a + 1;

            lv_obj_set_x(s->box, a);
            lv_obj_set_width(s->box, b - a);
            lv_obj_set_style_bg_color(s->box, lv_color_hex(theme_hex(colors[p->type])), 0);

            /* No label when it does not fit the visible part */
            for (int k = 0; k < 2 && labels[p->type][k] && !text; k++) {
                const char *t = labels[p->type][k];

                if (lv_txt_get_width(t, strlen(t), NF(16), 0, LV_TEXT_FLAG_NONE) + 6 <= b - a) {
                    text = t;
                }
            }
            if (text) {
                lv_label_set_text_static(s->label, text);
                lv_obj_center(s->label);
                lv_obj_clear_flag(s->label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s->label, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_clear_flag(s->box, LV_OBJ_FLAG_HIDDEN);
            n++;
        }
        pthread_mutex_unlock(&cur_lock);
    }
    for (; n < POOL; n++) {
        lv_obj_add_flag(segs[n].box, LV_OBJ_FLAG_HIDDEN);
    }
}

void band_plan_init(lv_obj_t *parent, lv_coord_t y) {
    band_plan_reload();
    for (int i = 0; i < POOL; i++) {
        lv_obj_t *b = lv_obj_create(parent);

        lv_obj_remove_style_all(b);
        lv_obj_set_y(b, y);
        lv_obj_set_height(b, BAND_PLAN_H);
        lv_obj_set_style_bg_opa(b, OPA, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        segs[i].box = b;

        lv_obj_t *l = lv_label_create(b);

        lv_obj_set_style_text_font(l, NF(16), 0);
        lv_obj_set_style_text_color(l, TC(bp_text), 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        segs[i].label = l;
    }
    ready = true;
    redraw();
}

void band_plan_set_y(lv_coord_t y) {
    for (int i = 0; i < POOL; i++) {
        if (segs[i].box) {
            lv_obj_set_y(segs[i].box, y);
        }
    }
}

void band_plan_update(int32_t center, int32_t half_width) {
    if (center == span_center && half_width == span_half) {
        return;
    }
    span_center = center;
    span_half   = half_width;
    redraw();
}

void band_plan_refresh(void) {
    redraw();
}

bool band_plan_segment(int i, int32_t *start, int32_t *end, int *type) {
    bool ok;

    pthread_mutex_lock(&cur_lock);
    ok = i >= 0 && i < ncur;
    if (ok) {
        *start = cur[i].start;
        *end   = cur[i].end;
        *type  = cur[i].type;
    }
    pthread_mutex_unlock(&cur_lock);
    return ok;
}

int band_plan_type(int32_t freq) {
    int type = -1;

    pthread_mutex_lock(&cur_lock);
    for (int i = 0; i < ncur; i++) {
        if (freq >= cur[i].start && freq < cur[i].end) {
            type = cur[i].type;
            break;
        }
    }
    pthread_mutex_unlock(&cur_lock);
    return type;
}

bool band_plan_is_cw(int32_t freq) {
    return band_plan_type(freq) == BP_CW;
}

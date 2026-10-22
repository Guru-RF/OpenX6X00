/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "band_plan.h"

#include "cfg/cfg_api.h"
#include "styles.h"
#include "theme.h"

#include <stddef.h>
#include <string.h>
#include "fonts/font_select.h"

/* VISION: the sizes of the other themes here (an overlay on VISION's shorter waterfall) */
#define NF(n) (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS) : &sony_##n)

#define WIDTH   800
#define POOL    12      /* segments in view: a span is 100 kHz at most */
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

static void redraw(void) {
    int n = 0;

    if (!ready) {
        return;
    }
    if (span_half > 0 && param_i_get(cfg_band_plan)) {
        int32_t left = span_center - span_half;
        int64_t span = 2 * (int64_t)span_half;

        for (size_t i = 0; i < sizeof(plan) / sizeof(plan[0]) && n < POOL; i++) {
            const bp_seg_t *p    = &plan[i];
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
    }
    for (; n < POOL; n++) {
        lv_obj_add_flag(segs[n].box, LV_OBJ_FLAG_HIDDEN);
    }
}

void band_plan_init(lv_obj_t *parent, lv_coord_t y) {
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
    if (i < 0 || (size_t)i >= sizeof(plan) / sizeof(plan[0]))
        return false;
    *start = plan[i].start;
    *end   = plan[i].end;
    *type  = plan[i].type;
    return true;
}

int band_plan_type(int32_t freq) {
    for (size_t i = 0; i < sizeof(plan) / sizeof(plan[0]); i++) {
        if (freq >= plan[i].start && freq < plan[i].end) {
            return plan[i].type;
        }
    }
    return -1;
}

bool band_plan_is_cw(int32_t freq) {
    for (size_t i = 0; i < sizeof(plan) / sizeof(plan[0]); i++) {
        if (freq >= plan[i].start && freq < plan[i].end) {
            return plan[i].type == BP_CW;
        }
    }
    return false;
}

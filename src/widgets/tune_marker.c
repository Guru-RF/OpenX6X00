/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - tuning marker over an app waterfall (lv_finder.h)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "tune_marker.h"
#include "../theme.h"

#define NO_CURSOR   (-1)

static void finder_init(const lv_obj_class_t * cls, lv_obj_t * obj);
static void finder_event(const lv_obj_class_t * cls, lv_event_t * e);

const lv_obj_class_t lv_finder_class = {
    .base_class     = &lv_obj_class,
    .constructor_cb = finder_init,
    .event_cb       = finder_event,
    .instance_size  = sizeof(lv_finder_t),
};

lv_obj_t * lv_finder_create(lv_obj_t * parent) {
    lv_obj_t *obj = lv_obj_class_create_obj(&lv_finder_class, parent);

    lv_obj_class_init_obj(obj);
    return obj;
}

void lv_finder_set_range(lv_obj_t * obj, int16_t min, int16_t max) {
    lv_finder_t *f = (lv_finder_t *)obj;

    f->range_min = min;
    f->range_max = max;
}

void lv_finder_set_cursor(lv_obj_t * obj, int16_t value) {
    ((lv_finder_t *)obj)->cursor = value;
}

void lv_finder_clear_cursor(lv_obj_t * obj) {
    ((lv_finder_t *)obj)->cursor = NO_CURSOR;
}

void lv_finder_set_width(lv_obj_t * obj, uint16_t x) {
    ((lv_finder_t *)obj)->width = x;
}

void lv_finder_set_value(lv_obj_t * obj, int16_t x) {
    ((lv_finder_t *)obj)->value = x;
}

static void finder_init(const lv_obj_class_t * cls, lv_obj_t * obj) {
    LV_UNUSED(cls);
    lv_finder_t *f = (lv_finder_t *)obj;

    f->range_min = 50;
    f->range_max = 3000;
    f->value = 1000;
    f->width = 50;
    f->cursor = NO_CURSOR;
}

/* Hz offset from range_min to pixels; the same integer rounding as the
 * band edges have always had, so the marker sits where it always sat */
static int px(int w, int hz, int span) {
    return w * hz / span;
}

static void draw_bands(lv_obj_t * obj, lv_draw_ctx_t * ctx) {
    lv_finder_t         *f = (lv_finder_t *)obj;
    int                 w = lv_obj_get_width(obj);
    int                 span = f->range_max - f->range_min;
    lv_coord_t          inset = lv_obj_get_style_border_width(obj, LV_PART_INDICATOR);
    lv_area_t           a;
    lv_draw_rect_dsc_t  d;

    a.y1 = obj->coords.y1 + inset;
    a.y2 = a.y1 + lv_obj_get_height(obj) - 2 * inset;
    a.x1 = obj->coords.x1 + px(w, f->value - f->range_min, span);
    a.x2 = obj->coords.x1 + px(w, f->value - f->range_min + f->width, span);

    lv_draw_rect_dsc_init(&d);
    lv_obj_init_draw_rect_dsc(obj, LV_PART_INDICATOR, &d);
    d.bg_color = d.border_color = TC(finder_band);
    d.bg_opa = LV_OPA_30;
    d.border_opa = LV_OPA_70;
    lv_draw_rect(ctx, &d, &a);

    if (f->cursor == NO_CURSOR || f->cursor == f->value) return;

    a.x1 = obj->coords.x1 + px(w, f->cursor - f->range_min, span);
    a.x2 = a.x1 + px(w, f->width, span);
    d.bg_color = d.border_color = TC(finder_cursor);
    lv_draw_rect(ctx, &d, &a);
}

static void finder_event(const lv_obj_class_t * cls, lv_event_t * e) {
    LV_UNUSED(cls);

    if (lv_obj_event_base(&lv_finder_class, e) != LV_RES_OK) return;

    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN_END) {
        draw_bands(lv_event_get_target(e), lv_event_get_draw_ctx(e));
    }
}

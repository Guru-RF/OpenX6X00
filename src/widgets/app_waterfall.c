/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - scrolling waterfall image for the apps (lv_waterfall.h)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "app_waterfall.h"

#include <string.h>

static void wf_init(const lv_obj_class_t * cls, lv_obj_t * obj);
static void wf_free(const lv_obj_class_t * cls, lv_obj_t * obj);

const lv_obj_class_t lv_waterfall_class = {
    .base_class     = &lv_img_class,
    .constructor_cb = wf_init,
    .destructor_cb  = wf_free,
    .instance_size  = sizeof(lv_waterfall_t),
};

lv_obj_t * lv_waterfall_create(lv_obj_t * parent) {
    lv_obj_t *obj = lv_obj_class_create_obj(&lv_waterfall_class, parent);

    lv_obj_class_init_obj(obj);
    return obj;
}

static void wf_init(const lv_obj_class_t * cls, lv_obj_t * obj) {
    LV_UNUSED(cls);
    lv_waterfall_t *wf = (lv_waterfall_t *)obj;

    memset(&wf->dsc, 0, sizeof(wf->dsc));
    wf->rows = NULL;
    wf->top = 0;
    wf->palette = NULL;
    wf->min = -40;
    wf->max = 0;
}

static void wf_free(const lv_obj_class_t * cls, lv_obj_t * obj) {
    LV_UNUSED(cls);
    lv_waterfall_t *wf = (lv_waterfall_t *)obj;

    if (wf->rows) {
        lv_img_cache_invalidate_src(&wf->dsc);
        lv_mem_free(wf->rows);
    }
    if (wf->palette) lv_mem_free(wf->palette);
}

void lv_waterfall_set_palette(lv_obj_t * obj, lv_color_t * palette, uint16_t cnt) {
    lv_waterfall_t *wf = (lv_waterfall_t *)obj;

    wf->palette = lv_mem_realloc(wf->palette, cnt * sizeof(lv_color_t));
    memcpy(wf->palette, palette, cnt * sizeof(lv_color_t));
#if LV_COLOR_DEPTH == 32
    /* wf_palette has alpha 0, which the DRM overlay shows as a hole */
    for (uint16_t i = 0; i < cnt; i++) wf->palette[i].ch.alpha = 0xff;
#endif
}

/* LVGL's lv_obj_invalidate() pads the area by a few pixels, and the padded
 * area no longer fits the opaque panel under the waterfall, so every row
 * would repaint the whole screen behind it. The image never draws outside
 * its own box: mark just that. */
static void redraw(lv_obj_t * obj) {
    lv_disp_t *disp = lv_obj_get_disp(obj);

    if (lv_disp_is_invalidation_enabled(disp) && lv_obj_is_visible(obj)) {
        _lv_inv_area(disp, &obj->coords);
    }
}

/* Black, not zero: zero is transparent on the DRM overlay */
static void blank(lv_waterfall_t * wf) {
    uint32_t n = 2u * wf->dsc.header.w * wf->dsc.header.h;

    for (uint32_t i = 0; i < n; i++) wf->rows[i] = lv_color_black();
}

void lv_waterfall_set_size(lv_obj_t * obj, lv_coord_t w, lv_coord_t h) {
    lv_waterfall_t *wf = (lv_waterfall_t *)obj;

    lv_obj_set_size(obj, w, h);

    if (wf->rows) {
        lv_img_cache_invalidate_src(&wf->dsc);
        lv_mem_free(wf->rows);
    }
    wf->rows = lv_mem_alloc(2u * w * h * sizeof(lv_color_t));
    LV_ASSERT_MALLOC(wf->rows);

    memset(&wf->dsc, 0, sizeof(wf->dsc));
    wf->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    wf->dsc.header.w = w;
    wf->dsc.header.h = h;
    wf->dsc.data_size = (uint32_t)w * h * sizeof(lv_color_t);
    wf->dsc.data = (const uint8_t *)wf->rows;
    wf->top = 0;
    blank(wf);

    lv_img_set_src(obj, &wf->dsc);
    lv_img_cache_invalidate_src(&wf->dsc);
}

void lv_waterfall_clear_data(lv_obj_t * obj) {
    lv_waterfall_t *wf = (lv_waterfall_t *)obj;

    if (!wf->rows) return;
    blank(wf);
    lv_img_cache_invalidate_src(&wf->dsc);
    redraw(obj);
}

void lv_waterfall_add_data(lv_obj_t * obj, float * data, uint16_t cnt) {
    lv_waterfall_t *wf = (lv_waterfall_t *)obj;

    if (!wf->rows || !wf->palette) return;

    uint32_t    w = wf->dsc.header.w;
    uint32_t    h = wf->dsc.header.h;
    float       span = wf->max - wf->min;

    wf->top = (wf->top + h - 1) % h;

    lv_color_t  *row = wf->rows + wf->top * w;

    for (uint32_t x = 0; x < w; x++) {
        float level = (data[x * cnt / w] - wf->min) / span;

        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;
        row[x] = wf->palette[(uint8_t)(level * 255)];
    }
    memcpy(row + h * w, row, w * sizeof(lv_color_t));

    wf->dsc.data = (const uint8_t *)row;
    lv_img_cache_invalidate_src(&wf->dsc);
    redraw(obj);
}

void lv_waterfall_add_data_with_ts(lv_obj_t * obj, float * data, uint16_t cnt, struct timespec ts) {
    LV_UNUSED(ts);
    lv_waterfall_add_data(obj, data, cnt);
}

void lv_waterfall_set_min(lv_obj_t * obj, int16_t val) {
    ((lv_waterfall_t *)obj)->min = val;
}

void lv_waterfall_set_max(lv_obj_t * obj, int16_t val) {
    ((lv_waterfall_t *)obj)->max = val;
}

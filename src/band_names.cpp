/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The bands around the VFO, named in boxes across the top of the waterfall for 2 s after the
 * frequency changes; not with the band plan strip on, which shows them already */

#include "band_names.h"

extern "C" {
#include "screen_light.h"
#include "gui_events.h"
#include "pubsub_ids.h"
#include "gui_styles.h"
#include "waterfall_view.h"
#include "lvgl/lvgl.h"
}

#include "cfg/cfg_api.h"
#include "cfg/db.h"
#include "cfg/subject.h"
#include "params/params.h"

#include <memory>
#include <vector>

#define STRIP_H  24
#define SHOW_MS  2000
#define FADE_MS  250
#define SPAN_HZ  100000     /* the waterfall at zoom 1 */

static lv_obj_t              *strip;
static std::vector<BandInfo>  bands;
static uint64_t               vfo;
static uint8_t                zoom = 1;
static int32_t                if_shift;
static lv_anim_t              fade;
static bool                   fading_in;
static lv_timer_t            *hide_timer;
static Subscription           zoom_sub, freq_sub, shift_sub;

/* x of f on the strip, w px wide, centred on the VFO as the waterfall is */
static int32_t band_x(int64_t f, lv_coord_t w, int32_t scale) {
    return (f - (int64_t)vfo + if_shift + waterfall_view_shift()) * w / SPAN_HZ * scale + w / 2;
}

static void draw_band(lv_draw_ctx_t *ctx, const lv_area_t *box, const BandInfo &band, int32_t scale) {
    lv_coord_t         w = lv_area_get_width(box), h = lv_area_get_height(box) - 1;
    int32_t            from = band_x(band.start_freq, w, scale), to = band_x(band.stop_freq, w, scale);
    lv_border_side_t   edges = LV_BORDER_SIDE_NONE;
    lv_draw_rect_dsc_t rect;
    lv_area_t          a;

    /* off the strip on a side: cut there, without the edge line */
    if (from > w || to < 0) {
        return;
    }
    if (from < 0) {
        from = 0;
    } else {
        edges |= LV_BORDER_SIDE_LEFT;
    }
    if (to > w) {
        to = w;
    } else {
        edges |= LV_BORDER_SIDE_RIGHT;
    }

    lv_draw_rect_dsc_init(&rect);
    rect.bg_color     = bg_color;
    rect.bg_opa       = LV_OPA_50;
    rect.border_width = 2;
    rect.border_color = theme_shape.mono ? TC(text) : lv_color_white();   /* 1bit: white is the paper */
    rect.border_opa   = LV_OPA_50;
    rect.border_side  = edges;
    a = (lv_area_t){(lv_coord_t)(box->x1 + from + 2), box->y1, (lv_coord_t)(box->x1 + to - 2),
                    (lv_coord_t)(box->y1 + h)};
    lv_draw_rect(ctx, &rect, &a);

    /* the name, when the box is wider than it */
    lv_draw_label_dsc_t text;
    lv_point_t          size;

    lv_draw_label_dsc_init(&text);
    text.color = theme_shape.mono ? TC(text) : lv_color_white();
    text.font  = &sony_22;
    lv_txt_get_size(&size, band.name.c_str(), text.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (to - from > size.x) {
        a.x1 = box->x1 + (from + to) / 2 - size.x / 2;
        a.y1 = box->y1 + (h - size.y) / 2;
        a.x2 = box->x1 + from + to / 2 + size.x / 2;    /* wider than the text: only a limit */
        a.y2 = box->y1 + h;
        lv_draw_label(ctx, &text, &a, band.name.c_str(), NULL);
    }
}

static void draw_cb(lv_event_t *e) {
    lv_obj_t      *o   = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);

    for (const BandInfo &band : bands) {
        draw_band(ctx, &o->coords, band, params.waterfall_zoom.x ? zoom : 1);
    }
}

static void set_opa(void *o, int32_t opa) {
    lv_obj_set_style_opa_layered((lv_obj_t *)o, opa, 0);
}

static void fade_done(lv_anim_t *a) {
    fading_in = false;
}

static void hide_cb(lv_timer_t *t) {
    hide_timer = NULL;
    lv_anim_set_values(&fade, lv_obj_get_style_opa(strip, 0), LV_OPA_TRANSP);
    lv_anim_start(&fade);
}

static void zoom_cb(Subject *subj, void *user_data) {
    zoom = static_cast<ParamInt *>(subj)->get();
    lv_obj_invalidate(strip);
}

static void freq_cb(Subject *subj, void *user_data) {
    band_info_update(static_cast<ComputedParamInt *>(subj)->get());
}

static void shift_cb(Subject *subj, void *user_data) {
    if_shift = static_cast<ParamInt *>(subj)->get();
    lv_obj_invalidate(strip);
}

extern "C" lv_obj_t *band_info_init(lv_obj_t *parent) {
    bands = BandsTable::all_bands();
    strip = lv_obj_create(parent);
    lv_obj_set_size(strip, lv_obj_get_width(parent), STRIP_H);
    lv_obj_align(strip, LV_ALIGN_TOP_MID, 0, 18 - STRIP_H / 2);    /* from the top: the waterfall's height changes */
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(strip, 0, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(strip, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, strip);
    lv_anim_set_time(&fade, FADE_MS);
    lv_anim_set_exec_cb(&fade, set_opa);
    lv_anim_set_ready_cb(&fade, fade_done);
    fading_in = false;

    /* subscribed once: a theme change makes the object again */
    if (!zoom_sub) {
        zoom_sub  = Subscription(cfg_mode_zoom->subscribe(zoom_cb, nullptr));
        freq_sub  = Subscription(cfg_fg_freq->subscribe_delayed(freq_cb, nullptr));
        shift_sub = Subscription(cfg_band_if_shift->subscribe_delayed(shift_cb, nullptr));
    }
    zoom_cb(cfg_mode_zoom, nullptr);
    freq_cb(cfg_fg_freq, nullptr);
    shift_cb(cfg_band_if_shift, nullptr);
    return strip;
}

extern "C" void band_info_reload(void) {
    bands = BandsTable::all_bands();
    if (strip) lv_obj_invalidate(strip);
}

extern "C" void band_info_update(int32_t freq) {
    vfo = freq;
    lv_obj_invalidate(strip);

    if (cfg_band_plan->get()) {
        lv_anim_del(strip, set_opa);
        fading_in = false;
        lv_obj_set_style_opa_layered(strip, LV_OPA_TRANSP, 0);
        return;
    }
    if (!fading_in) {
        fading_in = true;
        lv_anim_set_values(&fade, lv_obj_get_style_opa(strip, 0), LV_OPA_COVER);
        lv_anim_start(&fade);
    }
    if (hide_timer) {
        lv_timer_reset(hide_timer);
    } else {
        hide_timer = lv_timer_create(hide_cb, SHOW_MS, NULL);
        lv_timer_set_repeat_count(hide_timer, 1);
    }
}

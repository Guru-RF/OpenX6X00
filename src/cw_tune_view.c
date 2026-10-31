/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - CW tuning indicator: a row of bars under the frequency, the
 *  middle one for zero beat, each 10 Hz off one bar further; the bar hit lights up in
 *  green, yellow or red with the offset and fades back
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#include "cw_tune_view.h"

#include "gui_styles.h"
#include "theme.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "scheduler.h"

#include <math.h>
#include <time.h>

#define BARS        15
#define BAR_W       5
#define GAP         4
#define BOX_W       (BARS * (BAR_W + GAP) + GAP)
#define BOX_H       40
#define HZ_PER_BAR  10
#define LIT_MS      1000    /* lit for a second, */
#define FADE_MS     500     /* then back to the others in half a second */

static lv_obj_t             *box;
static lv_draw_rect_dsc_t   bar_dsc;            /* the bars at rest */
static lv_color_t           ok_color, near_color, off_color;
static lv_anim_t            fade;
static lv_coord_t           want_y = -1;       /* cw_tune_set_y(), kept for a box made later */

/* Written by the audio thread, drawn by the LVGL one */
static volatile int8_t      lit_bar = -100;     /* none before the first tone */
static lv_color_t           lit_color;
static volatile uint8_t     lit_level;          /* 255 just lit .. 0 faded out */
static volatile int         last_hz;
static volatile int64_t     last_ms = -10000;

static int64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fade_cb(void *var, int32_t level) {
    lit_level = level;
    lv_obj_invalidate(box);
}

/* LVGL thread: the fade starts over for each new tone (lv_anim_start drops the running one) */
static void relight(void *arg) {
    lv_anim_start(&fade);
    lv_obj_invalidate(box);
}

void cw_tune_set_freq(float hz) {
    float bar = BARS / 2 - roundf(hz / HZ_PER_BAR);
    float off = fabsf(hz);

    lit_color = off <= HZ_PER_BAR ? ok_color : off <= 2 * HZ_PER_BAR ? near_color : off_color;
    lit_bar = bar < 0 ? 0 : bar > BARS - 1 ? BARS - 1 : bar;
    last_hz = lroundf(hz);
    last_ms = now_ms();
    lit_level = 255;
    scheduler_put_noargs(relight);
}

void cw_tune_set_y(lv_coord_t y) {
    want_y = y;
    if (!box || !lv_obj_is_valid(box)) {
        return;     /* not made yet, or gone with the old theme */
    }
    if (y >= 0) {
        lv_obj_set_y(box, y);
    } else {
        lv_obj_remove_local_style_prop(box, LV_STYLE_Y, 0);
    }
}

bool cw_tune_recent(int *hz) {
    *hz = last_hz;
    return now_ms() - last_ms < LIT_MS;
}

/* Taller bars towards the middle; the lit one mixes its colour into the rest colour as it fades */
static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t       *ctx = lv_event_get_draw_ctx(e);
    lv_draw_rect_dsc_t  rest = bar_dsc;
    lv_draw_rect_dsc_t  lit = bar_dsc;
    lv_area_t           a;

    lit.bg_color = lv_color_mix(lit_color, bar_dsc.bg_color, lit_level);
    lit.bg_opa = bar_dsc.bg_opa + lit_level * (LV_OPA_70 - bar_dsc.bg_opa) / 255;

    /* See-through themes draw the box at half: the bars at half too */
    if (theme_shape.mono) {     /* 1bit: the bars hollow, the lit one solid until half faded; no mixes */
        rest.bg_opa = LV_OPA_TRANSP;
        rest.border_width = 1;
        rest.border_color = TC(text);
        rest.border_opa = LV_OPA_COVER;
        lit = rest;
        if (lit_level >= 128) {
            lit.bg_color = TC(text);
            lit.bg_opa = LV_OPA_COVER;
        }
    } else if (lv_obj_get_style_bg_opa(box, 0) != LV_OPA_COVER) {
        rest.bg_opa /= 2;
        lit.bg_opa /= 2;
    }

    for (int i = 0; i < BARS; i++) {
        lv_coord_t h = (BOX_H - 3) * 2 / (LV_ABS(i - BARS / 2) + 2);

        a.x1 = box->coords.x1 + GAP + i * (BAR_W + GAP);
        a.x2 = a.x1 + BAR_W;
        a.y2 = box->coords.y1 + BOX_H - 1;
        a.y1 = a.y2 - h;
        lv_draw_rect(ctx, i == lit_bar ? &lit : &rest, &a);
    }
}

/* Only in CW and CW-R, and only with the tune setting on; never over KX3Lovers Plus' display */
static void show_cb(Subject *subj, void *user_data) {
    radio_mode_t mode = cparam_i_get(cfg_cur_mode);

    if (param_i_get(cfg_cw_tune) && (mode == radio_mode_cw || mode == radio_mode_cwr) && !theme_kx3)
        lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
}

void cw_tune_init(lv_obj_t *parent) {
    ok_color = TC(ok);
    near_color = TC(tune_good);
    off_color = TC(err);

    lv_draw_rect_dsc_init(&bar_dsc);
    bar_dsc.bg_color = TC(text_mute);
    bar_dsc.bg_opa = LV_OPA_50;
    bar_dsc.radius = 5;

    box = lv_obj_create(parent);
    lv_obj_add_style(box, &cw_tune_style, 0);
    lv_obj_set_size(box, BOX_W, BOX_H);
    lv_obj_add_event_cb(box, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, box);
    lv_anim_set_exec_cb(&fade, fade_cb);
    lv_anim_set_values(&fade, 255, 0);
    lv_anim_set_delay(&fade, LIT_MS);
    lv_anim_set_time(&fade, FADE_MS);

    cw_tune_set_y(want_y);
    subject_subscribe_delayed((Subject *)cfg_cur_mode, show_cb, NULL);
    subject_subscribe_delayed_and_notify((Subject *)cfg_cw_tune, show_cb, NULL);
}

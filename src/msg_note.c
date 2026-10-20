/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The big short note in the middle (a value just changed: "AGC: Fast", the frequency):
 * a new text replaces the old at once and restarts its second on screen */

#include "msg_note.h"
#include "gui_styles.h"
#include "gui_events.h"

#include <stdio.h>

#define SHOW_MS 1000
#define FADE_MS 250
#define TOP_Y   (160 - 66 / 2 + 36 / 2)

static lv_obj_t   *label;
static char        text[512];
static lv_timer_t *hide_timer;
static lv_anim_t   fade;
static bool        fading_in;

static void set_opa(void *obj, int32_t opa) {
    lv_obj_set_style_opa(obj, opa, 0);
}

static void fade_done(lv_anim_t *a) {
    fading_in = false;
}

static void hide_cb(lv_timer_t *t) {
    hide_timer = NULL;
    lv_anim_set_values(&fade, lv_obj_get_style_opa(label, 0), LV_OPA_TRANSP);
    lv_anim_start(&fade);
}

static void show_cb(lv_event_t *e) {
    lv_coord_t w = lv_disp_get_hor_res(NULL), h = lv_disp_get_ver_res(NULL);

    /* Walkie Talkie: the display portrait, the top layer still landscape (as msg.c) */
    lv_obj_set_size(lv_obj_get_parent(label), w, h);
    lv_obj_set_style_max_width(label, w - 32, 0);   /* a long text wraps */
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, TOP_Y);
    lv_label_set_text(label, text);

    if (!fading_in) {
        fading_in = true;
        lv_anim_set_values(&fade, lv_obj_get_style_opa(label, 0), LV_OPA_COVER);
        lv_anim_start(&fade);
    }
    if (hide_timer) {
        lv_timer_reset(hide_timer);
    } else {
        hide_timer = lv_timer_create(hide_cb, SHOW_MS, NULL);
        lv_timer_set_repeat_count(hide_timer, 1);
    }
}

lv_obj_t *msg_tiny_init(lv_obj_t *parent) {
    label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, &msg_tiny_style, 0);
    lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);   /* at least the style's 324 x 66 */
    lv_obj_set_style_min_width(label, 324, 0);
    lv_obj_set_style_min_height(label, 66, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(label, show_cb, EVENT_MSG_UPDATE, NULL);
    lv_label_set_recolor(label, true);

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, label);
    lv_anim_set_time(&fade, FADE_MS);
    lv_anim_set_exec_cb(&fade, set_opa);
    lv_anim_set_ready_cb(&fade, fade_done);
    return label;
}

/* Any thread: the text is kept here, the GUI thread shows it */
void msg_tiny_set_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    event_send(label, EVENT_MSG_UPDATE, NULL);
}

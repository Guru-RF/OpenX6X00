/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The message box in the lower middle of the screen: a text fades in, stays, fades out.
 * Updates replace the text at once, scheduled ones wait for the ones before them. Any thread
 * may call: the text goes to the GUI through the event queue. */

#include "msg_box.h"
#include "gui_styles.h"
#include "util.h"
#include "gui_events.h"

#include <stdio.h>
#include <stdlib.h>

#define FADE_MS       250
#define SHOW_MS       2000
#define SHOW_LONG_MS  4000

typedef struct {
    char     text[128];
    bool     queued;        /* after the messages already waiting; else now */
    uint16_t ms;            /* on screen, fades included */
} note_t;

static lv_obj_t   *label;
static lv_anim_t   fade;
static lv_timer_t *hide_timer;
static uint32_t    free_at;     /* tick when the last message waiting will be gone */

static void set_opa(void *obj, int32_t opa) {
    lv_obj_set_style_opa(obj, opa, 0);
}

static void fade_to(lv_opa_t opa) {
    lv_anim_set_values(&fade, lv_obj_get_style_opa(label, 0), opa);
    lv_anim_start(&fade);
}

static void hide_cb(lv_timer_t *t) {
    hide_timer = NULL;
    fade_to(LV_OPA_TRANSP);
}

/* Walkie Talkie turns the display to portrait (480 x 800) while the top layer keeps the
 * landscape size: the layer as the display, the box in it, the text wrapped in portrait */
static void place(void) {
    lv_coord_t w = lv_disp_get_hor_res(NULL), h = lv_disp_get_ver_res(NULL);
    bool       portrait = h > w;

    lv_obj_set_size(lv_obj_get_parent(label), w, h);
    lv_obj_set_width(label, LV_MIN(theme_shape.vision ? 780 : 603, w - 32));   /* VISION's big type: most fit whole */
    lv_label_set_long_mode(label, portrait ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_SCROLL);
    lv_obj_align(label, portrait ? LV_ALIGN_CENTER : LV_ALIGN_TOP_MID, 0, portrait ? 0 : 270);
}

static void show_cb(lv_timer_t *t) {
    note_t *note = t->user_data;

    t->user_data = NULL;
    if (hide_timer) {
        lv_timer_del(hide_timer);
    }
    place();
    lv_label_set_text(label, note->text);
    lv_obj_move_foreground(label);
    fade_to(LV_OPA_COVER);
    hide_timer = lv_timer_create(hide_cb, note->ms - FADE_MS, NULL);
    lv_timer_set_repeat_count(hide_timer, 1);
    free(note);
}

/* In the GUI thread: a timer shows it now, or when the ones waiting are done */
static void arrived_cb(lv_event_t *e) {
    const note_t *in = lv_event_get_param(e);
    note_t       *note = malloc(sizeof(note_t));   /* the event's own is freed after this */
    uint32_t      now = lv_tick_get();
    uint32_t      wait = 0;

    if (free_at < now || !in->queued) {
        free_at = now;
    }
    if (in->queued) {
        wait = free_at - now;
    }
    *note = *in;
    lv_timer_set_repeat_count(lv_timer_create(show_cb, wait, note), 1);
    free_at += in->ms;
}

static void post(bool queued, uint16_t ms, const char *fmt, va_list ap) {
    note_t *note = malloc(sizeof(note_t));

    vsnprintf(note->text, sizeof(note->text), fmt, ap);
    note->queued = queued;
    note->ms = ms;

    event_send(label, EVENT_MSG_UPDATE, note);
}

lv_obj_t *msg_init(lv_obj_t *parent) {
    label = lv_label_create(parent);
    lv_obj_add_style(label, &msg_style, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(label, arrived_cb, EVENT_MSG_UPDATE, NULL);
    lv_label_set_recolor(label, true);   /* "#rrggbb text#" colours words */

    lv_anim_init(&fade);
    lv_anim_set_var(&fade, label);
    lv_anim_set_time(&fade, FADE_MS);
    lv_anim_set_exec_cb(&fade, set_opa);
    return label;
}

void msg_update_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    post(false, SHOW_MS, fmt, ap);
    va_end(ap);
}

void msg_schedule_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    post(true, SHOW_MS, fmt, ap);
    va_end(ap);
}

void msg_schedule_text_ms(uint16_t ms, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    post(true, ms, fmt, ap);
    va_end(ap);
}

void msg_schedule_long_text_fmt(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    post(true, SHOW_LONG_MS, fmt, ap);
    va_end(ap);
}

/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - common look of the fixed-frequency apps (WSPR,
 *  NavTex, WeFax, SSTV): a header (app and frequency on the left, state on
 *  the right), a thin cycle/progress bar, a black bordered body and a dim
 *  status line at the bottom, all on a full dialog.
 */

#pragma once

#include "lvgl/lvgl.h"
#include "theme.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_FRAME_X         13
#define APP_FRAME_W         771
#define APP_FRAME_BODY_Y    (10 + 34 + 10)
#define APP_FRAME_STATUS_H  30
#define APP_FRAME_BODY_H    (DIALOG_H - APP_FRAME_BODY_Y - APP_FRAME_STATUS_H - 8)

#define APP_FRAME_HEAD      theme_hex(TC_text_head)
#define APP_FRAME_DIM       theme_hex(TC_text_dim)

typedef struct {
    lv_obj_t *head_left;    /* recolor on */
    lv_obj_t *head_right;   /* recolor on */
    lv_obj_t *bar;          /* 0..1000, hidden until app_frame_bar() */
    lv_obj_t *status;       /* recolor on */
} app_frame_t;

/* Header, bar and status line on the dialog object */
void app_frame_create(app_frame_t *f, lv_obj_t *dialog_obj);

/* Puts obj where the body goes and gives it the black bordered look */
void app_frame_body(lv_obj_t *obj);

/* permille < 0 hides the bar */
void app_frame_bar(app_frame_t *f, int permille, uint32_t color);

#ifdef __cplusplus
}
#endif

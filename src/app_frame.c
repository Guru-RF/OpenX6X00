/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - common look of the fixed-frequency apps
 */

#include "app_frame.h"

#include "styles.h"

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_recolor(l, true);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

void app_frame_create(app_frame_t *f, lv_obj_t *dialog_obj) {
    f->head_left = make_label(dialog_obj, &sony_22, APP_FRAME_HEAD);
    lv_obj_set_pos(f->head_left, 18, 10);
    lv_obj_set_width(f->head_left, 480);

    f->head_right = make_label(dialog_obj, &sony_22, APP_FRAME_HEAD);
    lv_obj_set_width(f->head_right, 280);
    lv_obj_set_style_text_align(f->head_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(f->head_right, 796 - 18 - 280, 10);

    f->bar = lv_bar_create(dialog_obj);
    lv_obj_set_size(f->bar, APP_FRAME_W, 4);
    lv_obj_set_pos(f->bar, APP_FRAME_X, APP_FRAME_BODY_Y - 10);
    lv_bar_set_range(f->bar, 0, 1000);
    lv_obj_set_style_bg_color(f->bar, TC(track), LV_PART_MAIN);
    lv_obj_set_style_radius(f->bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(f->bar, 2, LV_PART_INDICATOR);
    lv_obj_add_flag(f->bar, LV_OBJ_FLAG_HIDDEN);

    f->status = make_label(dialog_obj, &sony_20, APP_FRAME_DIM);
    lv_obj_set_pos(f->status, 18, DIALOG_H - APP_FRAME_STATUS_H - 4);
    lv_obj_set_width(f->status, 760);
    lv_label_set_long_mode(f->status, LV_LABEL_LONG_SCROLL_CIRCULAR);   /* one line: a long one scrolls, never under the buttons */
}

void app_frame_body(lv_obj_t *obj) {
    lv_obj_set_align(obj, LV_ALIGN_TOP_LEFT);   /* a centred object would take the position as an offset */
    lv_obj_set_pos(obj, APP_FRAME_X, APP_FRAME_BODY_Y);
    lv_obj_set_size(obj, APP_FRAME_W, APP_FRAME_BODY_H);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, TC(dialog_border), LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, 96, LV_PART_MAIN);
}

void app_frame_bar(app_frame_t *f, int permille, uint32_t color) {
    if (permille < 0) {
        lv_obj_add_flag(f->bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(f->bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(f->bar, permille, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(f->bar, lv_color_hex(color), LV_PART_INDICATOR);
}

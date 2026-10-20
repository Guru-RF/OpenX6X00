/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Birdie Cal: with the antenna disconnected, Start scans every active band for
 *  the radio's own carriers (birdie.c) and keeps them; the waterfall marks them and the notch
 *  can take one out of the passband (Settings > Display > Birdies, also the Marks button here).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "dialog_birdie.h"

#include "birdie.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "events.h"
#include "fonts/font_select.h"
#include "keyboard.h"
#include "msg_box.h"
#include "radio.h"
#include "styles.h"
#include "theme.h"

#include <stdio.h>

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void start_cb(button_data_t *b);
static void clear_cb(button_data_t *b);
static void marks_cb(button_data_t *b);
static const char *start_label(void);
static const char *marks_label(void);

static button_data_t btn_start = {.type = BTN_TEXT_FN, .label_fn = start_label, .press = start_cb};
static button_data_t btn_clear = {.type = BTN_TEXT, .label = "Clear", .press = clear_cb};
static button_data_t btn_marks = {.type = BTN_TEXT_FN, .label_fn = marks_label, .press = marks_cb};

static buttons_page_t page = {{&btn_start, &btn_clear, &btn_marks}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
};

dialog_t *dialog_birdie = &dialog;

static lv_obj_t *status_label, *known_label;

static void known_update(void) {
    char s[96];
    int  n = birdie_count();

    if (!known_label) return;
    if (n == 0)
        snprintf(s, sizeof(s), "Known birdies: none yet.");
    else
        snprintf(s, sizeof(s), "Known birdies: %d, %d on this band.", n,
                 birdie_count_in(cparam_i_get(cfg_fg_freq) - 500000, cparam_i_get(cfg_fg_freq) + 500000));
    lv_label_set_text(known_label, s);
}

/* From the scan (LVGL thread) */
static void progress_cb(const char *text, bool done, void *arg) {
    (void)arg;
    if (!dialog.run) return;
    lv_label_set_text(status_label, text);
    if (done) {
        known_update();
        buttons_refresh(&btn_start);
    }
}

static const char *start_label(void) {
    return birdie_scanning() ? "Stop" : "Start";
}

static const char *marks_label(void) {
    static const char *const names[] = {"Marks:\nOff", "Marks:\nOn", "Marks:\n+ notch"};
    int32_t v = param_i_get(cfg_birdie_marks);
    return names[v >= 0 && v <= 2 ? v : 0];
}

static void start_cb(button_data_t *b) {
    (void)b;
    if (birdie_scanning()) {
        birdie_scan_stop();
        lv_label_set_text(status_label, "Stopping...");
        return;
    }
    if (radio_get_state() != RADIO_RX) {
        msg_update_text_fmt("Not while transmitting");
        return;
    }
    if (birdie_scan_start(progress_cb, NULL)) {
        lv_label_set_text(status_label, "Scanning. Keep the antenna disconnected.");
        buttons_refresh(&btn_start);
    }
}

static void clear_cb(button_data_t *b) {
    (void)b;
    if (birdie_scanning()) return;
    birdie_clear();
    known_update();
    lv_label_set_text(status_label, "Forgotten. Scan again when you like.");
}

static void marks_cb(button_data_t *b) {
    (void)b;
    param_i_set(cfg_birdie_marks, (param_i_get(cfg_birdie_marks) + 1) % 3);
    buttons_refresh(&btn_marks);
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t y, const char *s) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_width(l, 740);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, 20, y);
    lv_label_set_text(l, s);
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    buttons_load_page(&page);

    text(dialog.obj, &sony_28, TC(text_head), 12, "BIRDIE CALIBRATION");
    text(dialog.obj, &sony_22, TC(text), 56,
         "Birdies are steady carriers the radio makes inside itself. This scan finds them on every active "
         "band and keeps their frequencies: the waterfall marks them, and with Marks + notch the notch filter "
         "takes one out of the passband when you tune across it.");
    text(dialog.obj, &sony_22, TC(warn), 150, "Disconnect the antenna, then press Start. About a minute.");
    status_label = text(dialog.obj, &sony_24, TC(text_value), 200, "");
    known_label  = text(dialog.obj, &sony_22, TC(text_dim), 250, "");
    known_update();
    if (birdie_scanning())
        lv_label_set_text(status_label, "Scanning. Keep the antenna disconnected.");

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static void destruct_cb(void) {
    status_label = known_label = NULL;
    lv_group_remove_obj(dialog.obj);
}

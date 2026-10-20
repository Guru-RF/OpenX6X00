/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - frequency entry: F-INP on the hand mic or 'f' on a keyboard
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#include "freq_entry.h"

#include "gui_events.h"
#include "key_input.h"
#include "screen_main.h"
#include "msg_box.h"
#include "radio.h"
#include "gui_styles.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

static lv_obj_t *entry;

static void open_cb(lv_obj_t *parent);
static void entry_key_cb(lv_event_t *e);

static dialog_t dialog = { .construct_cb = open_cb, .key_cb = entry_key_cb };

dialog_t *dialog_freq = &dialog;

/* MHz as typed, "7.074" or ".5"; nothing typed changes nothing */
static void tune_to_entry(void) {
    const char  *mhz = lv_textarea_get_text(entry);
    int32_t     hz;

    if (!*mhz) return;
    hz = atof(mhz) * 1000000L;
    if (radio_check_freq(hz))
        main_screen_set_freq(hz);
    else
        msg_update_text_fmt("Incorrect freq");
}

/* F-INP again or ENTER tunes, ESC leaves it, VOL keeps the volume */
static void entry_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == HKEY_FINP || key == LV_KEY_ENTER) {
        tune_to_entry();
        dialog_destruct();
    } else if (key == LV_KEY_ESC) {
        dialog_destruct();
    } else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) {
        radio_change_vol(-1);
    } else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) {
        radio_change_vol(1);
    }
}

/* The small message box with one line of big digits across it */
static void open_cb(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &msg_tiny_style, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    dialog.obj = box;

    entry = lv_textarea_create(box);
    lv_obj_remove_style(entry, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_style_text_color(entry, TC(text), 0);
    lv_obj_set_style_bg_color(entry, TC(text), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(entry, LV_OPA_COVER, LV_PART_CURSOR);

    lv_textarea_set_one_line(entry, true);
    lv_textarea_set_accepted_chars(entry, "0123456789.");
    lv_textarea_set_max_length(entry, 9);
    lv_textarea_set_placeholder_text(entry, "Freq in MHz");
    lv_obj_clear_flag(entry, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(entry, &sony_44, 0);

    lv_group_add_obj(keyboard_group, entry);
    lv_obj_add_event_cb(entry, entry_key_cb, LV_EVENT_KEY, NULL);

    lv_obj_set_style_text_align(entry, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(entry, lv_pct(100), lv_font_get_line_height(&sony_44));
    lv_obj_center(entry);
}

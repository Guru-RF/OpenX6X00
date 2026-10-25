/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* A one-line text box in a popup, with the on-screen keyboard under it when no USB keyboard
 * is there. Enter (or F-INP, or the keyboard's tick) is OK, ESC cancels, VOL still sets the
 * volume. */

#include "text_popup.h"
#include "gui_events.h"
#include "radio.h"
#include "key_input.h"
#include "gui_styles.h"
#include "theme.h"

static lv_obj_t *window, *title, *field, *kbd;

static textarea_window_cb_t on_ok, on_cancel;

/* OK and Cancel arrive with their key still down. Deleting the focused window resets LVGL's
 * keypad with a press time of 0, so the key turned into a long press repeating into the object
 * focused next (an ESC here closed the whole app): it counts again only once released */
static void wait_key_up(void) {
    lv_indev_t *indev = lv_indev_get_act();

    if (indev) {
        lv_indev_wait_release(indev);
    }
}

static void accept(void) {
    wait_key_up();
    if (on_ok && on_ok()) {     /* false: the text is not right, the window stays */
        on_ok = NULL;
        textarea_window_close();
    }
}

static void dismiss(void) {
    wait_key_up();
    if (on_cancel) {
        on_cancel();
        on_cancel = NULL;
    }
    textarea_window_close();
}

/* The VOL knob, in both of its states */
static void volume_key(uint32_t key) {
    if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) {
        radio_change_vol(-1);
    } else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) {
        radio_change_vol(1);
    }
}

static void field_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ENTER || key == HKEY_FINP) {
        accept();
    } else if (key == LV_KEY_ESC) {
        dismiss();
    } else {
        volume_key(key);
    }
    /* before LVGL's own handler (PREPROCESS), which would type the key into the text */
    if (textarea_key_writes_nothing(key)) {
        lv_event_stop_processing(e);
    }
}

static void kbd_cb(lv_event_t *e) {
    switch (lv_event_get_code(e)) {
        case LV_EVENT_KEY: {
            uint32_t key = *(uint32_t *)lv_event_get_param(e);

            if (key == LV_KEY_ESC) {    /* the keys have the focus: ESC leaves as well */
                dismiss();
            } else {
                volume_key(key);
            }
            break;
        }
        case LV_EVENT_READY:
            accept();
            break;

        case LV_EVENT_CANCEL:
            dismiss();
            break;

        case LV_EVENT_FOCUSED:
            /* a long knob press leaves edit mode: the knob then moved the focus to the text
             * box instead of along the keys and the keyboard looked stuck */
            if (!lv_group_get_editing(keyboard_group)) {
                lv_group_set_editing(keyboard_group, true);
            }
            break;

        default:
            break;
    }
}

static void make_kbd(void) {
    static const lv_event_code_t codes[] = {LV_EVENT_READY, LV_EVENT_CANCEL, LV_EVENT_KEY, LV_EVENT_FOCUSED};

    kbd = lv_keyboard_create(lv_scr_act());
    lv_keyboard_set_textarea(kbd, field);
    lv_keyboard_set_mode(kbd, LV_KEYBOARD_MODE_TEXT_UPPER);
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        lv_obj_add_event_cb(kbd, kbd_cb, codes[i], NULL);
    }
    lv_obj_set_style_bg_color(kbd, bg_color, LV_PART_MAIN);
    lv_obj_add_style(kbd, &dialog_item_focus_style, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_group_add_obj(keyboard_group, kbd);
}

lv_obj_t *textarea_window_open(textarea_window_cb_t ok_cb, textarea_window_cb_t cancel_cb) {
    lv_obj_t *row, *title_box;

    on_ok     = ok_cb;
    on_cancel = cancel_cb;

    /* the box: a message box, higher up */
    window = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(window);
    lv_obj_add_style(window, &msg_style, 0);
    lv_obj_clear_flag(window, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_y(window, 80);

    /* a row: the title (empty unless given), then the text filling the rest */
    row = lv_obj_create(window);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_remove_style(row, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_height(row, 35);
    lv_obj_set_width(row, 560);
    lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    title_box = lv_obj_create(row);
    lv_obj_remove_style(title_box, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_size(title_box, LV_SIZE_CONTENT, LV_PCT(100));
    title = lv_label_create(title_box);
    lv_obj_set_style_text_font(title, &sony_36, 0);
    lv_label_set_text(title, "");
    lv_obj_align_to(title, title_box, LV_ALIGN_LEFT_MID, 0, 0);

    field = lv_textarea_create(row);
    lv_obj_remove_style(field, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_size(field, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(field, TC(text), 0);
    lv_obj_set_style_bg_color(field, TC(text), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(field, LV_OPA_80, LV_PART_CURSOR);
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_max_length(field, 64);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(field, &sony_44, 0);
    lv_obj_set_flex_grow(field, 1);
    if (ok_cb || cancel_cb) {
        lv_obj_add_event_cb(field, field_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    }

    kbd = NULL;
    if (!keyboard_ready()) {
        make_kbd();
    }
    lv_group_add_obj(keyboard_group, field);
    /* the focus here: opened from a list (JS8 APRS, status, saved messages) it stayed on the
     * list under the window and nothing could be typed */
    lv_group_focus_obj(kbd ? kbd : field);
    lv_group_set_editing(keyboard_group, true);
    return window;
}

lv_obj_t *textarea_window_open_w_label(textarea_window_cb_t ok_cb, textarea_window_cb_t cancel_cb, const char *text) {
    lv_obj_t *w = textarea_window_open(ok_cb, cancel_cb);

    lv_label_set_text(title, text);
    return w;
}

void textarea_window_close() {
    if (kbd) {
        lv_obj_del(kbd);
        kbd = NULL;
    }
    if (window) {
        lv_obj_del(window);
        window = NULL;
    }
}

bool textarea_window_is_open(void) {
    return window != NULL;
}

bool textarea_window_type(uint32_t key) {
    if (!window) {
        return false;
    }
    if (key == LV_KEY_BACKSPACE) {
        lv_textarea_del_char(field);
    } else {
        lv_textarea_add_char(field, key);
    }
    return true;
}

const char *textarea_window_get() {
    return lv_textarea_get_text(field);
}

void textarea_window_set(const char *val) {
    lv_textarea_set_text(field, val);
}

lv_obj_t *textarea_window_text() {
    return field;
}

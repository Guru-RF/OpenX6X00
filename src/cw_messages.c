/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  CW keyer: memory texts sent with the morse key, once or as a beacon.
 *  Page 1: TX, New, Edit; page 2: Beacon, Delete. Same layout as the
 *  voice keyer.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "app_window.h"
#include "cw_messages.h"
#include "gui_styles.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "gui_events.h"
#include "key_input.h"
#include "text_popup.h"
#include "cw_encoder.h"
#include "msg_box.h"
#include "button_bar.h"
#include "screen_main.h"
#include "radio.h"
#include "theme.h"

#define MAX_MSGS     64
#define TIMER_MS     100
#define CONFIRM_MS   3000

#define LIST_X       10
#define LIST_Y       40
#define LIST_W       470
#define LIST_H       (DIALOG_H - 86)
#define SIDE_X       494
#define SIDE_W       290

#define COLOR_HEAD   theme_hex(TC_text_head)
#define COLOR_DIM    theme_hex(TC_text_dim)
#define COLOR_SEL_BG theme_hex(TC_sel_bg)
#define COLOR_SEL    theme_hex(TC_sel_text)

typedef enum { EDIT_NONE, EDIT_NEW, EDIT_CHANGE } edit_t;

static uint32_t ids[MAX_MSGS];
static char    *texts[MAX_MSGS];
static int      count;
static int      sel;
static bool     beacon_on;
static edit_t   editing;
static uint32_t delete_armed;

static lv_obj_t   *list, *head, *head_right, *state_lbl, *text_lbl, *info_lbl, *hint;
static lv_timer_t *timer;

static void window_open(lv_obj_t *parent);
static void window_close(void);
static void tx_btn_cb(button_data_t *btn);
static void new_cb(button_data_t *btn);
static void edit_cb(button_data_t *btn);
static void close_cb(button_data_t *btn);
static void beacon_cb(button_data_t *btn);
static void delete_cb(button_data_t *btn);
static const char *tx_label(void);
static const char *beacon_label(void);

static button_data_t btn_p1 = {.type = BTN_TEXT, .label = "(MSG 1:2)", .press = button_next_page_cb};
static button_data_t btn_tx = {.type = BTN_TEXT_FN, .label_fn = tx_label, .press = tx_btn_cb};
static button_data_t btn_new = {.type = BTN_TEXT, .label = "New", .press = new_cb};
static button_data_t btn_edit = {.type = BTN_TEXT, .label = "Edit", .press = edit_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static button_data_t btn_p2 = {.type = BTN_TEXT, .label = "(MSG 2:2)", .press = button_next_page_cb};
static button_data_t btn_beacon = {.type = BTN_TEXT_FN, .label_fn = beacon_label, .press = beacon_cb};
static button_data_t btn_delete = {.type = BTN_TEXT, .label = "Delete", .press = delete_cb};
static button_data_t btn_close2 = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

buttons_page_t buttons_page_msg_cw_1 = {{&btn_p1, &btn_tx, &btn_new, &btn_edit, &btn_close}};
buttons_page_t buttons_page_msg_cw_2 = {{&btn_p2, &btn_beacon, &btn_delete, NULL, &btn_close2}};

static dialog_t dialog = { .construct_cb = window_open, .destruct_cb = window_close, .btn_page = &buttons_page_msg_cw_1 };

dialog_t *dialog_msg_cw = &dialog;

/* Messages */

static void clear_msgs(void) {
    for (int i = 0; i < count; i++) free(texts[i]);
    count = 0;
}

/* Called by params_msg_cw_load() for each stored text */
void dialog_msg_cw_append(uint32_t id, const char *val) {
    if (count >= MAX_MSGS || !val) return;
    ids[count]   = id;
    texts[count] = strdup(val);
    count++;
}

static void load_msgs(int select) {
    clear_msgs();
    params_msg_cw_load();

    lv_table_set_row_cnt(list, count > 0 ? count : 1);
    if (count == 0) lv_table_set_cell_value(list, 0, 0, "No messages yet: press New");
    for (int i = 0; i < count; i++) lv_table_set_cell_value(list, i, 0, texts[i]);

    sel = select < 0 ? count - 1 : select;
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    lv_label_set_text_fmt(head_right, "%i message%s", count, count == 1 ? "" : "s");
    lv_obj_invalidate(list);
}

static const char *sel_text(void) {
    return count > 0 ? texts[sel] : NULL;
}

/* Seconds to key a text: PARIS timing, 50 dits a word */
static float text_secs(const char *text) {
    float words = strlen(text) / 5.0f;
    return words * 60.0f / param_i_get(cfg_key_speed);
}

/* Buttons */

static bool sending(void) {
    return cw_encoder_state() != CW_ENCODER_IDLE;
}

static const char *tx_label(void) {
    return sending() ? "Stop" : "TX";
}

static const char *beacon_label(void) {
    static char buf[24];
    if (!beacon_on) return "Beacon\nOff";
    snprintf(buf, sizeof(buf), "Beacon\n%i s", params.cw_encoder_period);
    return buf;
}

static void set_btn(button_data_t *b, bool disabled) {
    if (b->disabled != disabled) buttons_disabled(b, disabled);
}

static void update_buttons(void) {
    bool busy  = sending();
    bool empty = count == 0;

    set_btn(&btn_tx, !busy && empty);
    set_btn(&btn_new, busy);
    set_btn(&btn_edit, busy || empty);
    set_btn(&btn_beacon, busy);
    set_btn(&btn_delete, busy || empty);
    set_btn(&btn_p1, busy);
    set_btn(&btn_p2, busy);

    static const char *tx_l;
    const char        *now = tx_label();
    if (now != tx_l) {
        tx_l = now;
        if (btn_tx.disp_btn) buttons_refresh(&btn_tx);
    }
}

/* Status panel */

static void set_state(const char *text, uint32_t bg, uint32_t fg) {
    lv_label_set_text(state_lbl, text);
    lv_obj_set_style_bg_color(state_lbl, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(fg), 0);
}

static void update_status(void) {
    const char *text = sel_text();
    int         wpm  = param_i_get(cfg_key_speed);

    lv_label_set_text_fmt(head, "CW KEYER - #%06x %i WPM#", theme_hex(TC_text), wpm);
    lv_label_set_text(text_lbl, text ? text : "-");

    switch (cw_encoder_state()) {
        case CW_ENCODER_SEND:
        case CW_ENCODER_BEACON:
            set_state("ON AIR", theme_hex(TC_tx_bg), theme_hex(TC_text));
            lv_label_set_text(hint, "TX or PTT: stop");
            break;
        case CW_ENCODER_BEACON_IDLE: {
            char buf[32];
            snprintf(buf, sizeof(buf), "BEACON %i s", cw_encoder_pause_left());
            set_state(buf, theme_hex(TC_chip), theme_hex(TC_warn));
            lv_label_set_text(hint, "TX or PTT: stop");
            break;
        }
        default:
            set_state("READY", theme_hex(TC_chip), theme_hex(TC_text_head));
            lv_label_set_text(hint, count ? "MFK: choose - press MFK: edit" : "New: write the first message");
            break;
    }

    if (text)
        lv_label_set_text_fmt(info_lbl, "About %.0f s at %i WPM\nBeacon: %s", text_secs(text), wpm,
                              beacon_on ? "on" : "off (page 2)");
    else
        lv_label_set_text(info_lbl, "");
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    if (delete_armed && lv_tick_elaps(delete_armed) > CONFIRM_MS) delete_armed = 0;
    update_status();
    update_buttons();
}

/* List */

static void list_draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS) return;
    uint32_t r = dsc->id;

    dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
    dsc->rect_dsc->bg_color = TC(panel);
    if (count == 0) {
        dsc->label_dsc->color = lv_color_hex(COLOR_DIM);
        return;
    }
    if ((int)r == sel) {
        dsc->rect_dsc->bg_color     = lv_color_hex(COLOR_SEL_BG);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 2;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM;
        dsc->label_dsc->color       = lv_color_hex(COLOR_SEL);
    }
}

static void move_sel(int step) {
    if (sending() || count == 0) return;
    sel += step;
    if (sel < 0) sel = 0;
    if (sel >= count) sel = count - 1;

    lv_table_t *t = (lv_table_t *)list;
    lv_coord_t  y = 0;
    for (int i = 0; i < sel; i++) y += t->row_h[i];
    lv_obj_scroll_to_y(list, y + t->row_h[sel] / 2 - LIST_H / 2, LV_ANIM_OFF);
    lv_obj_invalidate(list);
    delete_armed = 0;
}

/* Keys */

/* Arrows move in the list, ENTER or F4 edits, ESC closes, VOL keeps the volume */
static void key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_LEFT || key == LV_KEY_UP)
        move_sel(-1);
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN)
        move_sel(1);
    else if (key == LV_KEY_ENTER || key == KEYBOARD_F4)
        edit_cb(NULL);
    else if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}

/* MFK press: the encoder sends a click, not ENTER. */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) edit_cb(NULL);
}

/* PTT stops a beacon during its pause. */
static void radio_tx_cb(lv_event_t *e) {
    (void)e;
    if (cw_encoder_state() == CW_ENCODER_BEACON_IDLE) cw_encoder_stop();
}

/* Text editing */

static void edit_end(void) {
    textarea_window_close();
    editing = EDIT_NONE;
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static bool edit_ok(void) {
    const char *val = textarea_window_get();
    if (!val || !*val) {
        msg_update_text_fmt("Empty message");
        return false;
    }
    edit_t what = editing;
    if (what == EDIT_NEW) params_msg_cw_new(val);
    else params_msg_cw_edit(ids[sel], val);
    int keep = what == EDIT_NEW ? -1 : sel;
    edit_end();
    load_msgs(keep);
    return true;
}

static bool edit_cancel(void) {
    edit_end();
    return true;
}

static void open_editor(edit_t what) {
    if (sending() || editing != EDIT_NONE) return;
    if (what == EDIT_CHANGE && !sel_text()) return;

    editing = what;
    /* The text area gets the keys only when the dialog is out of the group. */
    lv_group_remove_obj(dialog.obj);
    textarea_window_open(edit_ok, edit_cancel);
    if (what == EDIT_CHANGE) textarea_window_set(sel_text());
}

/* Button actions */

static void tx_btn_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) cw_encoder_stop();
    else if (sel_text()) cw_encoder_send(cw_encoder_expand(sel_text()), beacon_on);
    timer_cb(NULL);
}

static void new_cb(button_data_t *btn) {
    (void)btn;
    open_editor(EDIT_NEW);
}

static void edit_cb(button_data_t *btn) {
    (void)btn;
    open_editor(EDIT_CHANGE);
}

static void close_cb(button_data_t *btn) {
    (void)btn;
    dialog_destruct();
}

/* Off, then the pause after each sending: past the last one back to off */
static const int beacon_pauses[] = { 10, 30, 60, 120 };

/* The first longer pause after the starting one; 0 past the last */
static int next_pause(int now) {
    for (size_t i = 1; i <sizeof(beacon_pauses) / sizeof(beacon_pauses[0]); i++)
        if (beacon_pauses[i] > now) return beacon_pauses[i];
    return 0;
}

static void beacon_cb(button_data_t *btn) {
    (void)btn;
    if (sending()) return;

    params_lock();
    int pause = beacon_on ? next_pause(params.cw_encoder_period) : beacon_pauses[0];

    beacon_on = pause != 0;
    if (beacon_on) params.cw_encoder_period = pause;
    params_unlock(&params.dirty.cw_encoder_period);
    buttons_refresh(&btn_beacon);
    timer_cb(NULL);
}

static void delete_cb(button_data_t *btn) {
    (void)btn;
    if (sending() || !sel_text()) return;

    if (!delete_armed) {
        delete_armed = lv_tick_get();
        msg_update_text_fmt("Press Delete again to delete the message");
        return;
    }
    params_msg_cw_delete(ids[sel]);
    delete_armed = 0;
    msg_update_text_fmt("Message deleted");
    load_msgs(sel);
    timer_cb(NULL);
}

/* Dialog */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void window_open(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    btn_p1.next = btn_p1.prev = &buttons_page_msg_cw_2;
    btn_p2.next = btn_p2.prev = &buttons_page_msg_cw_1;

    head = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_label_set_recolor(head, true);
    lv_obj_set_pos(head, 16, 10);
    head_right = label(dialog.obj, &sony_20, COLOR_HEAD);
    lv_obj_set_width(head_right, 250);
    lv_obj_set_style_text_align(head_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(head_right, 796 - 16 - 250, 10);

    list = lv_table_create(dialog.obj);
    lv_obj_remove_style(list, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(list, LIST_W, LIST_H);
    lv_obj_set_pos(list, LIST_X, LIST_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, theme_shape.border, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, TC(panel_border), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(list, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(list, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(list, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(list, 10, LV_PART_ITEMS);
    lv_table_set_col_cnt(list, 1);
    lv_table_set_col_width(list, 0, LIST_W - 2);
    lv_obj_add_event_cb(list, list_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    lv_obj_t *side = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, SIDE_W, LIST_H);
    lv_obj_set_pos(side, SIDE_X, LIST_Y);
    lv_obj_set_style_bg_color(side, TC(side), 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(side, TC(panel_border), 0);
    lv_obj_set_style_border_width(side, theme_shape.border, 0);
    lv_obj_set_style_radius(side, 8, 0);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    state_lbl = label(side, &sony_28, COLOR_HEAD);
    lv_obj_set_size(state_lbl, SIDE_W - 24, 48);
    lv_obj_set_pos(state_lbl, 12, 12);
    lv_obj_set_style_bg_opa(state_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(state_lbl, 8, 0);
    lv_obj_set_style_text_align(state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(state_lbl, 8, 0);

    text_lbl = label(side, &sony_20, theme_hex(TC_text_value));
    lv_obj_set_width(text_lbl, SIDE_W - 24);
    lv_label_set_long_mode(text_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(text_lbl, 12, 72);

    info_lbl = label(side, &sony_18, COLOR_DIM);
    lv_obj_set_width(info_lbl, SIDE_W - 24);
    lv_obj_set_pos(info_lbl, 12, 196);

    hint = label(dialog.obj, &sony_18, COLOR_DIM);
    lv_obj_set_pos(hint, 16, DIALOG_H - 32);

    lv_obj_add_event_cb(dialog.obj, radio_tx_cb, EVENT_RADIO_TX, NULL);
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    load_msgs(0);
    main_screen_lock_mode(true);
    timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
    timer_cb(NULL);
}

static void window_close(void) {
    cw_encoder_stop();
    if (editing != EDIT_NONE) {
        textarea_window_close();
        editing = EDIT_NONE;
    }
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    lv_group_remove_obj(dialog.obj);
    clear_msgs();
    delete_armed = 0;
    main_screen_lock_mode(false);
}

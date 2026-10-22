// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - the CHANNELS window (dialog_channels.h).
//
// A one-column table, one row per channel: the MFK moves the cursor, a press tunes the
// channel and closes the window. Each cell holds a blank and the row is painted at the end
// of its draw (name on the left, frequency and mode on the right), so the cursor and the
// scrolling stay LVGL's own. Keys: Close, Add, Delete, Edit (name) and Drag: while
// dragging the MFK carries the channel up and down the list, Drop writes the new order.
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#include "dialog_channels.h"     /* dialog_channels */

#include <stdio.h>       /* snprintf */

#include "channels.h"            /* the list and its file */
#include "keyboard.h"            /* keyboard_group */
#include "textarea_window.h"     /* the name editor */
#include "cfg/cfg_api.h"         /* the current frequency */
#include "styles.h"              /* fonts */
#include "radio_types.h"
#include "theme.h"

static void window_open_cb(lv_obj_t *parent);
static void window_close_cb(void);
static void key_close_cb(button_data_t *data);
static void key_add_cb(button_data_t *data);
static void key_delete_cb(button_data_t *data);
static void key_edit_cb(button_data_t *data);
static void key_drag_cb(button_data_t *data);
static void key_drop_cb(button_data_t *data);

static button_data_t btn_close  = { .type = BTN_TEXT, .label = "Close",  .press = key_close_cb };
static button_data_t btn_add    = { .type = BTN_TEXT, .label = "Add",    .press = key_add_cb };
static button_data_t btn_delete = { .type = BTN_TEXT, .label = "Delete", .press = key_delete_cb };
static button_data_t btn_edit   = { .type = BTN_TEXT, .label = "Edit",   .press = key_edit_cb };
static button_data_t btn_drag   = { .type = BTN_TEXT, .label = "Drag",   .press = key_drag_cb };
static button_data_t btn_drop   = { .type = BTN_TEXT, .label = "Drop",   .press = key_drop_cb };

static buttons_page_t keys_page = {
    .items = { &btn_close, &btn_add, &btn_delete, &btn_edit, &btn_drag }
};

static dialog_t win = {
    .btn_page       = &keys_page,
    .construct_cb   = window_open_cb,
    .destruct_cb    = window_close_cb,
};

dialog_t *dialog_channels = &win;   /* the hand mic V/M key opens it */

static lv_obj_t *table;

/* Rows, -1 for none. recalled lives as long as the GUI (not saved): the window
   opens on it, the hand mic UP/DOWN step from it */
static int16_t recalled = -1;
static int16_t renaming = -1;       /* row whose name is in the keyboard window */
static int16_t renamed = -1;        /* row to put the cursor back on after a rename */
static int16_t dragging = -1;       /* row the dragged channel is on, -1 when not dragging */

/* Mode as shown in the list: "?" for one the file can't hold */
static const char *mode_label(radio_mode_t mode) {
    const char *name = channels_mode_name(mode);
    radio_mode_t back;

    return channels_mode_parse(name, &back) && back == mode ? name : "?";
}

static void fill_table(void) {
    uint16_t n = channels_count();

    if (!table) return;
    /* One empty row when there are no channels: LVGL crashes on a table cut to zero rows */
    for (uint16_t i = 0; i < (n ? n : 1); i++) lv_table_set_cell_value(table, i, 0, " ");
    lv_table_set_row_cnt(table, n ? n : 1);
}

static void draw_row_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (!dsc || dsc->part != LV_PART_ITEMS) return;

    lv_obj_t *obj = lv_event_get_target(e);
    uint16_t cols = lv_table_get_col_cnt(obj);

    if (!cols) return;

    const channel_t *c = channels_get(dsc->id / cols);

    if (!c) return;

    lv_draw_label_dsc_t text = *dsc->label_dsc;
    const lv_area_t *cell = dsc->draw_area;
    lv_area_t a = {
        .y1 = cell->y1 + lv_obj_get_style_pad_top(obj, LV_PART_ITEMS),
        .y2 = cell->y2 - lv_obj_get_style_pad_bottom(obj, LV_PART_ITEMS),
    };
    char mhz[24];

    text.opa = LV_OPA_COVER;

    a.x1 = cell->x1 + lv_obj_get_style_pad_left(obj, LV_PART_ITEMS);
    a.x2 = cell->x2 - 340;
    text.align = LV_TEXT_ALIGN_LEFT;
    lv_draw_label(dsc->draw_ctx, &text, &a, c->name, NULL);

    snprintf(mhz, sizeof(mhz), "%u.%03u.%03u", c->freq / 1000000, c->freq / 1000 % 1000, c->freq % 1000);
    a.x1 = cell->x2 - 335;
    a.x2 = cell->x2 - 110;
    text.align = LV_TEXT_ALIGN_RIGHT;
    lv_draw_label(dsc->draw_ctx, &text, &a, mhz, NULL);

    a.x1 = cell->x2 - 105;
    a.x2 = cell->x2 - 12;
    lv_draw_label(dsc->draw_ctx, &text, &a, mode_label(c->mode), NULL);
}

/* Row under the cursor, -1 when there is none */
static int16_t cursor_row(void) {
    uint16_t row = LV_TABLE_CELL_NONE, col;

    if (!table) return -1;
    lv_table_get_selected_cell(table, &row, &col);
    return row < channels_count() ? row : -1;
}

static void give_table_the_mfk(void) {
    lv_group_add_obj(keyboard_group, table);
    lv_group_focus_obj(table);
    lv_group_set_editing(keyboard_group, true);
}

static void moved_cb(lv_event_t *e);
static void pressed_cb(lv_event_t *e);

/* A new table each time: rows are never left empty after a delete. The cursor starts on row 0 */
static void make_table(void) {
    if (table != NULL) {
        lv_group_remove_obj(table);
        lv_obj_del(table);
    }
    table = lv_table_create(win.obj);
    lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
    lv_obj_set_size(table, 700, 270);
    lv_obj_align(table, LV_ALIGN_TOP_MID, 0, 50);
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, 690);
    lv_obj_set_style_bg_color(table, TC(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &sony_24, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 12, LV_PART_ITEMS);
    /* The cursor row: dimmed text, no highlight */
    lv_obj_set_style_text_color(table, TC(text_mute), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_EDITED);

    fill_table();
    lv_obj_add_event_cb(table, pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(table, moved_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(table, draw_row_cb, LV_EVENT_DRAW_PART_END, NULL);
    give_table_the_mfk();
}

/* Move the cursor to a row as the MFK would, optionally scrolling it to mid-list */
static void cursor_to(int16_t row, bool middle) {
    if (!table || row < 0 || row >= channels_count()) return;

    for (int16_t i = 0; i < row; i++) lv_group_send_data(keyboard_group, LV_KEY_DOWN);
    lv_obj_update_layout(table);
    if (!middle) return;

    lv_coord_t h = lv_font_get_line_height(lv_obj_get_style_text_font(table, LV_PART_ITEMS))
        + lv_obj_get_style_pad_top(table, LV_PART_ITEMS) + lv_obj_get_style_pad_bottom(table, LV_PART_ITEMS);
    lv_coord_t y = row * h + h / 2 - lv_obj_get_content_height(table) / 2;

    lv_obj_scroll_to_y(table, y < 0 ? 0 : y, LV_ANIM_OFF);
}

static bool tune(int16_t row, bool close) {
    const channel_t *c = row < 0 ? NULL : channels_get(row);

    if (!c) return false;

    recalled = row;
    cparam_i_set(cfg_fg_freq, c->freq);
    cparam_i_set(cfg_cur_mode, c->mode);
    cparam_i_set(cfg_cur_agc, c->agc);
    cparam_i_set(cfg_cur_pre, c->pre ? radio_pre_on : radio_pre_off);
    cparam_i_set(cfg_cur_att, c->att ? radio_att_on : radio_att_off);
    if (close) dialog_destruct();
    return true;        /* tuned */
}

static void set_f5(button_data_t *btn) {
    keys_page.items[4] = btn;
    buttons_load(4, btn);
}

static void pressed_cb(lv_event_t *e) {
    if (dragging >= 0) return;

    uint16_t row = LV_TABLE_CELL_NONE, col;

    lv_table_get_selected_cell(table, &row, &col);
    if (row != LV_TABLE_CELL_NONE) tune(row, true);
}

/* While dragging, the channel follows the cursor; the order is saved on Drop */
static void moved_cb(lv_event_t *e) {
    int16_t to = dragging < 0 ? -1 : cursor_row();
    int16_t from = dragging;

    if (to < 0 || to == from || !channels_move(from, to)) return;

    if (recalled == from)
        recalled = to;
    else if (from < to && recalled > from && recalled <= to)
        recalled--;
    else if (from > to && recalled >= to && recalled < from)
        recalled++;
    dragging = to;
    fill_table();
}

static void key_close_cb(button_data_t *data) {
    dialog_destruct();      /* window_close_cb does the rest */
}

static void key_add_cb(button_data_t *data) {
    if (dragging >= 0 || !channels_add_current()) return;

    make_table();
    cursor_to(channels_count() - 1, false);
    lv_obj_scroll_to_y(table, LV_COORD_MAX, LV_ANIM_OFF);
}

static void key_delete_cb(button_data_t *data) {
    int16_t row = dragging >= 0 ? -1 : cursor_row();

    if (row < 0 || !channels_delete(row)) return;

    if (recalled == row)
        recalled = -1;
    else if (recalled > row)
        recalled--;

    make_table();

    int16_t n = channels_count();
    int16_t next = row > 0 ? row - 1 : 0;

    if (n > 0) cursor_to(next < n ? next : n - 1, true);
}

static void rename_done(void *arg) {
    int16_t row = renamed;

    renamed = -1;
    if (!win.run || !win.obj) return;
    make_table();
    cursor_to(row, true);
}

static bool rename_ok_cb(void) {
    if (renaming < 0 || renaming >= channels_count()) {
        renaming = -1;
        give_table_the_mfk();
        return true;    /* nothing to rename: the keyboard closes */
    }
    if (!channels_set_name(renaming, textarea_window_get())) return false;

    /* The table is rebuilt once the keyboard window is done with its own OK */
    renamed = renaming;
    renaming = -1;
    lv_async_call(rename_done, NULL);
    return true;        /* the keyboard closes */
}

static bool rename_cancel_cb(void) {
    renaming = -1;
    give_table_the_mfk();
    return true;        /* closes, the name as it was */
}

static void key_edit_cb(button_data_t *data) {
    int16_t row = dragging >= 0 ? -1 : cursor_row();

    if (row < 0) return;

    renaming = row;
    lv_group_remove_obj(table);
    textarea_window_open(rename_ok_cb, rename_cancel_cb);
    lv_textarea_set_max_length(textarea_window_text(), CHANNEL_NAME_MAX - 1);
    lv_textarea_set_placeholder_text(textarea_window_text(), "Channel name");
    textarea_window_set(channels_get(row)->name);
}

static void key_drag_cb(button_data_t *data) {
    int16_t row = cursor_row();

    if (row < 0) return;
    dragging = row;
    set_f5(&btn_drop);
}

static void key_drop_cb(button_data_t *data) {
    if (dragging < 0) return;

    /* A failed write: go back to what the file holds */
    bool saved = channels_save();

    if (!saved) channels_load();
    dragging = -1;
    set_f5(&btn_drag);
    if (!saved) make_table();
}

static void window_open_cb(lv_obj_t *parent) {
    channels_load();        /* the file may have changed since the last time */
    dragging = -1;
    keys_page.items[4] = &btn_drag;

    /* Same size and frame as the WEFAX big view; dialog_init's own background is turned
       off or its outline would show under the frame */
    win.obj = dialog_init(parent);
    lv_obj_set_size(win.obj, 795, 350);
    lv_obj_center(win.obj);
    lv_obj_set_style_bg_img_src(win.obj, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(win.obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(win.obj, 0, LV_PART_MAIN);

    lv_obj_t *frame = lv_obj_create(win.obj);

    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, 795, 350);
    lv_obj_center(frame);
    lv_obj_set_style_bg_color(frame, TC(frame), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(frame, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(frame, TC(dialog_border), LV_PART_MAIN);
    lv_obj_set_style_border_opa(frame, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(frame, 8, LV_PART_MAIN);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(frame);

    lv_obj_t *title = lv_label_create(win.obj);

    lv_label_set_text(title, "CHANNELS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    make_table();

    cursor_to(recalled, true);
}

static void window_close_cb(void) {
    table = NULL;
    renaming = -1;
    dragging = -1;
    keys_page.items[4] = &btn_drag;
}

void dialog_channels_poll(void) {
    if (!win.run) channels_reload_if_changed();
}

const char *dialog_channels_last_recalled_name(void) {    /* see dialog_channels.h */
    static char numbered[32];
    const channel_t *c = recalled < 0 ? NULL : channels_get(recalled);

    if (!c) return "";
    if (c->name[0]) return c->name;
    snprintf(numbered, sizeof(numbered), "CHANNEL %d", recalled + 1);
    return numbered;
}

bool dialog_channels_recall_relative(int8_t step) {
    /* Read the file every time: the hand mic UP/DOWN keys work before the window was ever
       opened and see edits made by hand */
    if (!step || !channels_load()) return false;

    int16_t n = channels_count();
    int16_t row = 0;

    if (n <= 0) return false;
    if (recalled < 0 || recalled >= n)
        row = step < 0 ? n - 1 : 0;
    else
        row = (recalled + (step < 0 ? n - 1 : 1)) % n;

    if (!tune(row, false)) return false;

    /* The window is open: rebuild it with the cursor on the new channel */
    if (win.run && win.obj && table) {
        make_table();
        cursor_to(recalled, true);
    }
    return true;
}

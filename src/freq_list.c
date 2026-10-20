/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Frequency bookmarks list, shared by the WeFax and NavTex apps.
 */

#include "freq_list.h"

#include "cfg/cfg_api.h"
#include "events.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "radio.h"
#include "styles.h"
#include "theme.h"
#include "freq_strip.h"

#include <stdio.h>
#include <stdlib.h>

#define WIDTH      771
#define HEADER_Y   12
#define TABLE_Y    44
#define TABLE_H    (DIALOG_H - 86)
#define COLOR_HEAD theme_hex(TC_text_head)
#define COLOR_GRP  theme_hex(TC_text_head)
#define COLOR_DIM  theme_hex(TC_text_dim)
#define COLOR_SEL  theme_hex(TC_sel_bg)
#define COLOR_CUR  theme_hex(TC_near_bg)
#define COLOR_NEAR theme_hex(TC_sel_text)

static lv_obj_t               *panel;
static lv_obj_t               *table;
static lv_obj_t               *prev_focus;
static const freq_list_item_t *items;
static int                     count;
static int                     sel;
static int32_t                 audio_hz;
static void                    (*on_close)(void);

static int32_t item_dial(const freq_list_item_t *it, int32_t audio) {
    return it->lsb ? it->freq_hz + audio : it->freq_hz - audio;
}

static int32_t dial_of(int i) {
    return item_dial(&items[i], audio_hz);
}

static bool is_current(int i) {
    return !items[i].group && dial_of(i) == cparam_i_get(cfg_fg_freq);
}

static void step(int dir) {
    for (int i = sel + dir; dir && i >= 0 && i < count; i += dir)
        if (!items[i].group) {
            sel = i;
            break;
        }
    if (sel < 0) return;
    /* Keeps the selection in the middle, a header above it stays visible. */
    lv_table_t *t = (lv_table_t *)table;
    lv_coord_t  y = 0;
    for (int i = 0; i < sel; i++) y += t->row_h[i];
    lv_obj_scroll_to_y(table, y + t->row_h[sel] / 2 - TABLE_H / 2, LV_ANIM_OFF);
    lv_obj_invalidate(table);
}

static void tune(void) {
    if (sel < 0 || items[sel].group) return;
    freq_list_tune(&items[sel], audio_hz);
    freq_list_close();
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            step(-1);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            step(1);
            break;
        case LV_KEY_ENTER:
            tune();
            break;
        case LV_KEY_ESC:
            freq_list_close();
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

/* MFK press: the encoder sends a click, not ENTER. */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) tune();
}

/* A tap on a row tunes to it. */
static void table_click_cb(lv_event_t *e) {
    (void)e;
    uint16_t r, c;
    lv_table_get_selected_cell(table, &r, &c);
    if (r >= count || items[r].group) return;
    sel = r;
    tune();
}

static void draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS) return;
    uint32_t r = dsc->id / 4, c = dsc->id % 4;
    if (r >= (uint32_t)count) return;

    dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
    dsc->rect_dsc->bg_color = TC(panel);
    if (items[r].group) {
        dsc->label_dsc->color = lv_color_hex(COLOR_GRP);
        return;
    }
    if ((int)r == sel) {
        dsc->rect_dsc->bg_color     = lv_color_hex(COLOR_SEL);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 2;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM | (c == 0 ? LV_BORDER_SIDE_LEFT : 0) |
                                     (c == 3 ? LV_BORDER_SIDE_RIGHT : 0);
    } else if (is_current(r)) {
        dsc->rect_dsc->bg_color = lv_color_hex(COLOR_CUR);
    }
    if (c == 1) dsc->label_dsc->color = lv_color_hex(COLOR_DIM);
    if (c == 2 && ((int)r == sel || is_current(r))) dsc->label_dsc->color = lv_color_hex(COLOR_NEAR);
    if (c == 3) dsc->label_dsc->color = TC(text_value);
}

void freq_list_open(lv_obj_t *parent, const char *title, const freq_list_item_t *list, int n, int32_t audio,
                    void (*close_cb)(void)) {
    if (panel) return;
    items    = list;
    count    = n;
    audio_hz = audio;
    on_close = close_cb;

    panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_add_style(panel, &dialog_style, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* dialog_style places it on the screen: inside an app's dialog (SSTV, NAVTEX), already at
     * DIALOG_Y, it went down twice and its bottom under the buttons */
    lv_area_t pa;
    lv_obj_update_layout(parent);
    lv_obj_get_content_coords(parent, &pa);   /* children are placed from the padding in */
    lv_obj_set_pos(panel, lv_obj_get_style_x(panel, 0) - pa.x1, lv_obj_get_style_y(panel, 0) - pa.y1);

    int32_t   vfo  = cparam_i_get(cfg_fg_freq);
    lv_obj_t *head = lv_label_create(panel);
    lv_obj_set_style_text_font(head, &sony_20, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(COLOR_HEAD), 0);
    lv_label_set_recolor(head, true);
    lv_label_set_text_fmt(head, "%s #%06x - VFO %d.%03d.%02d#", title, theme_hex(TC_text), vfo / 1000000, (vfo / 1000) % 1000,
                          (vfo % 1000) / 10);
    lv_obj_set_pos(head, 18, HEADER_Y);

    lv_obj_t *hint = lv_label_create(panel);
    lv_obj_set_style_text_font(hint, &sony_18, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text(hint, "MFK: choose - press: tune");
    lv_obj_set_pos(hint, 18, DIALOG_H - 30);

    table = lv_table_create(panel);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(table, WIDTH, TABLE_H);
    lv_obj_set_pos(table, 13, TABLE_Y);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, TC(dialog_border), LV_PART_MAIN);
    lv_obj_set_style_border_opa(table, 96, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(table, 8, LV_PART_ITEMS);
    lv_table_set_col_cnt(table, 4);
    lv_table_set_row_cnt(table, (uint16_t)n);
    static const lv_coord_t w[4] = {185, 165, 165, WIDTH - 2 - 515};
    for (int c = 0; c < 4; c++) lv_table_set_col_width(table, (uint16_t)c, w[c]);

    char text[32];
    sel = -1;
    for (int i = 0; i < n; i++) {
        if (items[i].group) {
            lv_table_set_cell_value(table, (uint16_t)i, 0, items[i].group);
            lv_table_add_cell_ctrl(table, (uint16_t)i, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT | LV_TABLE_CELL_CTRL_TEXT_CROP);
            lv_table_add_cell_ctrl(table, (uint16_t)i, 1, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
            lv_table_add_cell_ctrl(table, (uint16_t)i, 2, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
            continue;
        }
        int32_t f = items[i].freq_hz, d = dial_of(i);
        snprintf(text, sizeof(text), "%d.%d", f / 1000, (f % 1000) / 100);
        lv_table_set_cell_value(table, (uint16_t)i, 0, text);
        snprintf(text, sizeof(text), "dial %d.%d", d / 1000, (d % 1000) / 100);
        lv_table_set_cell_value(table, (uint16_t)i, 1, text);
        lv_table_set_cell_value(table, (uint16_t)i, 2, items[i].name);
        lv_table_set_cell_value(table, (uint16_t)i, 3, items[i].note ? items[i].note : "");
        if (sel < 0 || is_current(i)) sel = i;
    }
    lv_obj_add_event_cb(table, draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, table_click_cb, LV_EVENT_CLICKED, NULL);

    /* The list takes the MFK while it is open. */
    prev_focus = lv_group_get_focused(keyboard_group);
    lv_group_add_obj(keyboard_group, panel);
    lv_obj_add_event_cb(panel, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(panel, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(panel);
    lv_group_set_editing(keyboard_group, true);

    step(0);
}

void freq_list_close(void) {
    if (!panel) return;
    lv_group_remove_obj(panel);
    lv_obj_del(panel);
    panel = NULL;
    table = NULL;
    if (prev_focus) {
        lv_group_focus_obj(prev_focus);
        lv_group_set_editing(keyboard_group, true);
    }
    if (on_close) on_close();
}

void freq_list_tune(const freq_list_item_t *it, int32_t audio) {
    int32_t dial = item_dial(it, audio);

    main_screen_set_freq(dial);
    cparam_i_set(cfg_cur_mode, it->lsb ? x6100_mode_lsb : x6100_mode_usb);
    /* The station stays named in the frequency strip while decoding */
    char label[64];
    if (it->note)
        snprintf(label, sizeof(label), "%s - %s", it->name, it->note);
    else
        snprintf(label, sizeof(label), "%s", it->name);
    freq_strip_set_label(label, dial);
    msg_update_text_fmt("%s %d.%d kHz", it->name, it->freq_hz / 1000, (it->freq_hz % 1000) / 100);
}

const freq_list_item_t *freq_list_base(const freq_list_item_t *list, int n, int32_t audio, int32_t vfo,
                                       int32_t *dial) {
    const freq_list_item_t *best = NULL;
    int32_t                 dist = 0;

    for (int i = 0; i < n; i++) {
        if (list[i].group) continue;
        int32_t d = abs(item_dial(&list[i], audio) - vfo);
        if (!best || d < dist) {
            best = &list[i];
            dist = d;
        }
    }
    if (best && dial) *dial = item_dial(best, audio);
    return best;
}

bool freq_list_is_open(void) {
    return panel != NULL;
}

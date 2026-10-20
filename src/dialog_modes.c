/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  APP "Modes" menu: one button for all the digital mode apps. The MFK
 *  chooses, a press opens the app; Modes again closes the menu.
 */

#include "dialog_modes.h"

#include "buttons.h"
#include "events.h"
#include "keyboard.h"
#include "main_screen.h"
#include "params/params.h"
#include "radio.h"
#include "styles.h"
#include "theme.h"
#include "cfg/cfg_api.h"
#include "dialog_psk.h"

#include <ft8lib/constants.h>

#include <stdint.h>

#define BOX_W  760
#define COL_W  ((BOX_W - 4) / 2)
#define ROW_H  37
#define HEAD_H 38
#define HINT_H 28
#define BOX_Y  (TOP_BAR_STD_H + 4)
#define ROWS_N ((MODES_N + 1) / 2)    /* two columns, filled top to bottom, left first */
#define BOX_H  (HEAD_H + ROWS_N * ROW_H + HINT_H + 8)

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);

static const struct {
    const char    *name;
    const char    *note;
    press_action_t action;
    int            ftx; /* FT8 app protocol, PSK app: 0 PSK31, 1 PSK63, -1 = neither */
} modes[] = {
    {"RTTY", "45 bd", ACTION_APP_RTTY, -1},
    {"JTTY", "1.9 s", ACTION_APP_JTTY, -1},
    {"PSK31", "31 bd", ACTION_APP_PSK, 0},
    {"PSK63", "63 bd", ACTION_APP_PSK, 1},
    {"FT8", "15 s", ACTION_APP_FT8, FTX_PROTOCOL_FT8},
    {"FT4", "7.5 s", ACTION_APP_FT8, FTX_PROTOCOL_FT4},
    {"FT2", "3.75 s", ACTION_APP_FT8, 2 /* FTX_PROTOCOL_FT2, ft8/ft2.h */},
    {"WSPR", "2 min", ACTION_APP_WSPR, -1},
    {"Olivia", "8/250", ACTION_APP_OLIVIA, -1},
    {"JS8", "", ACTION_APP_JS8, -1},
    {"SSTV", "", ACTION_APP_SSTV, -1},
    {"WeFax", "", ACTION_APP_WEFAX, -1},
    {"NavTex", "", ACTION_APP_NAVTEX, -1},
};

#define MODES_N ((int)(sizeof(modes) / sizeof(modes[0])))

/* 80 + 333 = 413 with 2 x 7 rows of 37: stays clear of the buttons (480 - BUTTONS_H_MAX = 418). */
_Static_assert(BOX_Y + BOX_H <= 480 - BUTTONS_H_MAX, "Modes menu overlaps the buttons");

static lv_obj_t *rows[MODES_N];
static int       sel;
static int       last_used = -1;

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .key_cb       = NULL,
};

dialog_t *dialog_modes = &dialog;

static void show_sel(void) {
    for (int i = 0; i < MODES_N; i++) {
        bool on = i == sel;
        lv_obj_set_style_bg_opa(rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rows[i], on ? 2 : 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(rows[i], 0), on ? TC(sel_text) : TC(text), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(rows[i], 1), on ? TC(sel_text) : TC(text_dim), 0);
    }
}

static void start_async(void *action) {
    main_screen_start_app((press_action_t)(intptr_t)action);
}

static void open_sel(void) {
    last_used = sel;
    /* The app is started after this event: starting it closes this menu. */
    if (modes[sel].ftx >= 0 && modes[sel].action == ACTION_APP_FT8) {
        param_i_set(cfg_ft8_protocol, modes[sel].ftx);
    }
    if (modes[sel].ftx >= 0 && modes[sel].action == ACTION_APP_PSK) {
        dialog_psk_set_mode(modes[sel].ftx);
    }
    lv_async_call(start_async, (void *)(intptr_t)modes[sel].action);
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            if (sel > 0) sel--;
            show_sel();
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            if (sel < MODES_N - 1) sel++;
            show_sel();
            break;
        case LV_KEY_ENTER:
            open_sel();
            break;
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

/* MFK press: the encoder sends a click, not ENTER. */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) open_sel();
}

static void row_click_cb(lv_event_t *e) {
    sel = (int)(intptr_t)lv_event_get_user_data(e);
    show_sel();
    open_sel();
}

static void construct_cb(lv_obj_t *parent) {
    /* The APP buttons stay: Modes closes the menu, another app replaces it. */
    dialog.btn_page = dialog.prev_page;
    if (dialog.btn_page) buttons_load_page(dialog.btn_page);

    lv_obj_t *box = lv_obj_create(parent);
    dialog.obj    = box;
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, BOX_W, BOX_H);
    lv_obj_set_pos(box, (800 - BOX_W) / 2, BOX_Y);
    lv_obj_add_style(box, &popup_style, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_label_create(box);
    lv_obj_set_style_text_font(head, &sony_22, 0);
    lv_obj_set_style_text_color(head, TC(text_head), 0);
    lv_label_set_text(head, "DIGITAL MODES");
    lv_obj_set_pos(head, 22, 9);

    for (int i = 0; i < MODES_N; i++) {
        lv_obj_t *row = lv_obj_create(box);
        rows[i]       = row;
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, COL_W, ROW_H);
        lv_obj_set_pos(row, (i / ROWS_N) * COL_W, HEAD_H + (i % ROWS_N) * ROW_H);
        lv_obj_set_style_bg_color(row, TC(sel_bg), 0);
        lv_obj_set_style_border_color(row, TC(sel_border), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_font(name, &sony_30, 0);
        lv_label_set_text(name, modes[i].name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 22, 0);

        lv_obj_t *note = lv_label_create(row);
        lv_obj_set_style_text_font(note, &sony_22, 0);
        lv_obj_set_style_text_color(note, TC(text_dim), 0);
        lv_label_set_text(note, i == last_used ? "last used" : modes[i].note);
        lv_obj_align(note, LV_ALIGN_RIGHT_MID, -22, 0);
    }

    lv_obj_t *hint = lv_label_create(box);
    lv_obj_set_style_text_font(hint, &sony_20, 0);
    lv_obj_set_style_text_color(hint, TC(text_dim), 0);
    lv_label_set_text(hint, "MFK: choose - press: open - Modes: close");
    lv_obj_set_pos(hint, 22, HEAD_H + ROWS_N * ROW_H + 8);

    sel = last_used < 0 ? 0 : last_used;
    show_sel();

    lv_group_add_obj(keyboard_group, box);
    lv_obj_add_event_cb(box, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(box, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(box);
    lv_group_set_editing(keyboard_group, true);
}

static void destruct_cb(void) {
    lv_group_remove_obj(dialog.obj);
}

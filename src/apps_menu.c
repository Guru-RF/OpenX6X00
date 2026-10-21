/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the APP menu as a grid of icons: the apps of the three APP button pages as
 *  tiles, four to a row. The MFK moves along the tiles, a press opens the one chosen, ESC or
 *  the APP key closes the grid. The button bar keeps the page it had, so the text buttons
 *  work as before underneath. Settings > Display > Icon menus turns the grid off.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "apps_menu.h"

#include "buttons.h"
#include "cfg/cfg_api.h"
#include "events.h"
#include "fonts/font_select.h"
#include "keyboard.h"
#include "params/params.h"
#include "main_screen.h"
#include "radio.h"
#include "screen_rec.h"
#include "styles.h"
#include "theme.h"

#include <stdint.h>

#define COLS   4
#define GAP    4
#define PAD    6
#define BOX_W  760
#define TILE_W ((BOX_W - 2 * PAD - (COLS - 1) * GAP) / COLS)
#define TILE_H 66
#define HEAD_H 38
#define HINT_H 26
#define BOX_Y  (TOP_BAR_STD_H + 4)
#define ICON   36

typedef enum { OPEN_APP, OPEN_SCREEN_REC, OPEN_WALKIE } open_t;

static const struct {
    const char    *name, *sub, *icon;
    open_t         what;
    press_action_t action;
} apps[] = {
    {"Modes", "FT8, JS8, PSK, SSTV...", "modes", OPEN_APP, ACTION_APP_MODES},
    {"Cluster", "DX spots", "cluster", OPEN_APP, ACTION_APP_CLUSTER},
    {"Field Ops", "SOTA, POTA, WWFF", "field", OPEN_APP, ACTION_APP_FIELD},
    {"Bands", "Band activity", "bands", OPEN_APP, ACTION_APP_BANDS},
    {"Channels", "Memory channels", "channels", OPEN_APP, ACTION_APP_CHANNELS},
    {"SWR Scan", "Sweep, transmits", "swr", OPEN_APP, ACTION_APP_SWRSCAN},
    {"Recorder", "RX audio", "recorder", OPEN_APP, ACTION_APP_RECORDER},
    {"CW Trainer", "Learn CW", "trainer", OPEN_APP, ACTION_APP_CWTRAINER},
    {"GPS", "Position, time", "gps", OPEN_APP, ACTION_APP_GPS},
    {"Screen Rec", "Screen and audio", "screenrec", OPEN_SCREEN_REC, 0},
    {"Walkie", "Portrait screen", "walkie", OPEN_WALKIE, 0},
    {"Birdie Cal", "Find birdies, antenna off", "birdie", OPEN_APP, ACTION_APP_BIRDIE},
    {"Settings", "All settings", "settings", OPEN_APP, ACTION_APP_SETTINGS},
};

#define APPS_N ((int)(sizeof(apps) / sizeof(apps[0])))
#define ROWS_N ((APPS_N + COLS - 1) / COLS)
#define BOX_H  (HEAD_H + ROWS_N * TILE_H + (ROWS_N - 1) * GAP + HINT_H + PAD)

_Static_assert(BOX_Y + BOX_H <= 480 - BUTTONS_H_MAX, "APP grid overlaps the buttons");

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);

static lv_obj_t *tiles[APPS_N];
static int       sel;

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .key_cb       = NULL,
};

dialog_t *dialog_apps = &dialog;

static void show_sel(void) {
    for (int i = 0; i < APPS_N; i++) {
        bool on = i == sel;

        lv_obj_set_style_border_color(tiles[i], on ? TC(sel_border) : TC(panel_border), 0);
        lv_obj_set_style_border_width(tiles[i], on ? 2 : 1, 0);
        lv_obj_set_style_bg_color(tiles[i], on ? TC(sel_bg) : TC(chip), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tiles[i], 1), on ? TC(sel_text) : TC(text), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tiles[i], 2), on ? TC(sel_text) : TC(text_dim), 0);
        lv_obj_set_style_img_recolor(lv_obj_get_child(tiles[i], 0), on ? TC(sel_text) : TC(accent), 0);
    }
}

static void open_async(void *arg) {
    int i = (int)(intptr_t)arg;

    switch (apps[i].what) {
        case OPEN_APP:
            main_screen_start_app(apps[i].action);  /* closes the grid first */
            break;
        case OPEN_SCREEN_REC:
            dialog_destruct();
            screen_rec_toggle();
            break;
        case OPEN_WALKIE:
            dialog_destruct();
            param_i_set(cfg_walkie, 1);             /* walkie.c follows the setting */
            break;
    }
}

static void open_sel(void) {
    /* After this event: what opens closes this grid */
    lv_async_call(open_async, (void *)(intptr_t)sel);
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_LEFT:
            if (sel > 0) sel--;
            show_sel();
            break;
        case LV_KEY_RIGHT:
            if (sel < APPS_N - 1) sel++;
            show_sel();
            break;
        case LV_KEY_UP:
            if (sel >= COLS) sel -= COLS;
            show_sel();
            break;
        case LV_KEY_DOWN:
            if (sel + COLS < APPS_N) sel += COLS;
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

/* MFK press: the encoder sends a click, not ENTER */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) open_sel();
}

static void tile_click_cb(lv_event_t *e) {
    sel = (int)(intptr_t)lv_event_get_user_data(e);
    show_sel();
    open_sel();
}

static void construct_cb(lv_obj_t *parent) {
    /* The buttons stay as they are: the grid is a way in, not a page */
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
    lv_label_set_text(head, "APPS");
    lv_obj_set_pos(head, 22, 9);

    for (int i = 0; i < APPS_N; i++) {
        lv_obj_t *t = lv_obj_create(box);
        tiles[i]    = t;
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, TILE_W, TILE_H);
        lv_obj_set_pos(t, PAD + (i % COLS) * (TILE_W + GAP), HEAD_H + (i / COLS) * (TILE_H + GAP));
        lv_obj_set_style_radius(t, 8, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* child 0 the icon (a label stands in when the file is missing), 1 the name, 2 the line */
        lv_obj_t *img = theme_icon_img(t, apps[i].icon, ICON, TC(accent));
        if (!img) {
            img = lv_label_create(t);
            lv_label_set_text(img, "");
        }
        lv_obj_set_pos(img, 10, (TILE_H - ICON) / 2);

        /* The name, then the line in two rows of a smaller font: beside the icon the tile is
         * narrow (117 px) and the lines wrap; with one row's height the second was cut off */
        lv_obj_t *name = lv_label_create(t);
        lv_obj_set_style_text_font(name, &sony_22, 0);
        lv_label_set_text(name, apps[i].name);
        lv_obj_set_pos(name, 10 + ICON + 10, 5);

        lv_obj_t *sub = lv_label_create(t);
        lv_obj_set_style_text_font(sub, &sony_16, 0);
        lv_label_set_text(sub, apps[i].sub);
        lv_obj_set_size(sub, TILE_W - ICON - 30, 36);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(sub, 10 + ICON + 10, 29);
    }

    lv_obj_t *hint = lv_label_create(box);
    lv_obj_set_style_text_font(hint, &sony_20, 0);
    lv_obj_set_style_text_color(hint, TC(text_dim), 0);
    lv_label_set_text(hint, "MFK: choose - press: open - APP or ESC: close");
    lv_obj_set_pos(hint, 22, HEAD_H + ROWS_N * TILE_H + (ROWS_N - 1) * GAP + 4);

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

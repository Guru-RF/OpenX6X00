/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the APP menu as a grid of icons: the apps of the three APP button pages as
 *  tiles, four to a row, four rows in sight: more scroll up into them, the row chosen always in
 *  sight. The MFK moves along the tiles, a press opens the one chosen, ESC or the APP key closes
 *  the grid. The button bar keeps the page it had, so the text buttons work as before
 *  underneath. Settings > Display > Icon menus turns the grid off. The apps hidden in Settings >
 *  Apps have no tile, the others close up (cfg/apps_list.h, #351).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "apps_menu.h"

#include "buttons.h"
#include "cfg/apps_list.h"
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
#include "voice.h"
#include "voice_focus.h"

#include <stdint.h>

#define COLS   4
#define GAP    4
#define PAD    6
#define BOX_W  796     /* the screen less the top bar's margins: nothing of the strip shows at the sides */
#define TILE_W ((BOX_W - 2 * PAD - (COLS - 1) * GAP) / COLS)
#define TILE_H 66
#define HEAD_H 38
#define HINT_H 26
#define BOX_Y  (TOP_BAR_STD_H + 4)
#define ICON   36
#define VIEW_ROWS 4    /* rows in sight: a fifth would reach under the buttons */

typedef enum { OPEN_APP, OPEN_SCREEN_REC, OPEN_WALKIE } open_t;

static const struct {
    const char    *name, *sub, *icon;
    open_t         what;
    press_action_t action;
    app_id_t       id;      /* its name in the hidden apps' setting (cfg/apps_list.h) */
} apps[] = {
    {"Modes", "FT8, JS8, PSK, SSTV", "modes", OPEN_APP, ACTION_APP_MODES, APPID_MODES},
    {"Cluster", "DX spots", "cluster", OPEN_APP, ACTION_APP_CLUSTER, APPID_CLUSTER},
    {"Field Ops", "SOTA, POTA, WWFF", "field", OPEN_APP, ACTION_APP_FIELD, APPID_FIELD},
    {"Bands", "Band activity", "bands", OPEN_APP, ACTION_APP_BANDS, APPID_BANDS},
    {"Propagation", "Conditions, beacons", "prop", OPEN_APP, ACTION_APP_PROPAGATION, APPID_PROPAGATION},
    {"Channels", "Memory channels", "channels", OPEN_APP, ACTION_APP_CHANNELS, APPID_CHANNELS},
    {"Scanner", "Stops on activity", "scan", OPEN_APP, ACTION_APP_SCAN, APPID_SCAN},
    {"SWR Scan", "Sweep, transmits", "swr", OPEN_APP, ACTION_APP_SWRSCAN, APPID_SWRSCAN},
    {"Recorder", "RX audio", "recorder", OPEN_APP, ACTION_APP_RECORDER, APPID_RECORDER},
    {"CW Trainer", "Learn CW", "trainer", OPEN_APP, ACTION_APP_CWTRAINER, APPID_CWTRAINER},
    {"CW Keyboard", "CW as you type", "text", OPEN_APP, ACTION_APP_CW_KBD, APPID_CW_KBD},   /* the keyboard of RTTY, PSK */
    {"Mail", "Winlink, no phone", "mail", OPEN_APP, ACTION_APP_MAIL, APPID_MAIL},
    {"GPS / Clock", "Position, time", "gps", OPEN_APP, ACTION_APP_GPS, APPID_GPS},
    {"Screen Rec", "Screen and audio", "screenrec", OPEN_SCREEN_REC, 0, APPID_SCREEN_REC},
    {"Walkie", "Portrait screen", "walkie", OPEN_WALKIE, 0, APPID_WALKIE},
    {"Birdie Cal", "Find birdies, antenna off", "birdie", OPEN_APP, ACTION_APP_BIRDIE, APPID_BIRDIE},
    {"Web radio", "Internet stations", "iradio", OPEN_APP, ACTION_APP_IRADIO, APPID_IRADIO},   /* not "Internet radio": cut in 1bit */
    {"Zello", "Internet voice", "zello", OPEN_APP, ACTION_APP_ZELLO, APPID_ZELLO},
    {"SVXConnect", "Reflector talkgroups", "svx", OPEN_APP, ACTION_APP_SVX, APPID_SVX},
    {"DMR", "Talkgroups, listen only", "dmr", OPEN_APP, ACTION_APP_DMR, APPID_DMR},
    {"Flights", "Aircraft around", "flights", OPEN_APP, ACTION_APP_FLIGHTS, APPID_FLIGHTS},
    {"HF Air Band", "Oceanic routes, VOLMET", "aero", OPEN_APP, ACTION_APP_AERO, APPID_AERO},
    {"CB", "27 MHz channels", "cb", OPEN_APP, ACTION_APP_CB, APPID_CB},
    {"Weather", "Forecast, warnings", "meteo", OPEN_APP, ACTION_APP_METEO, APPID_METEO},
    {"Settings", "All settings", "settings", OPEN_APP, ACTION_APP_SETTINGS, APPID_SETTINGS},
};

#define APPS_N ((int)(sizeof(apps) / sizeof(apps[0])))
/* The grid of n tiles: its rows in sight, the height of its view and of the box. With apps hidden the box is as
 * low as its rows */
#define ROWS_V(n) (((n) + COLS - 1) / COLS < VIEW_ROWS ? ((n) + COLS - 1) / COLS : VIEW_ROWS)
#define GRID_H(n) (ROWS_V(n) * TILE_H + (ROWS_V(n) - 1) * GAP)
#define BOX_H(n)  (HEAD_H + GRID_H(n) + HINT_H + PAD)

_Static_assert(BOX_Y + BOX_H(APPS_N) <= BOARD_DISP_H - BUTTONS_H_MAX, "APP grid overlaps the buttons");
_Static_assert(APPS_N == APPID_COUNT, "an app of the grid is not in cfg/apps_list.h, or the other way round");

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);

static lv_obj_t *tiles[APPS_N];   /* the tiles shown, closed up */
static uint8_t   shown[APPS_N];   /* their apps: places in apps[] */
static int       shown_n;
static int       sel;             /* the tile chosen, a place in shown[] */
static int       sel_app;         /* its app, kept from one opening to the next */

uint32_t apps_menu_hidden(void) {
    char text[APPS_HIDDEN_LEN];

    param_t_get(cfg_apps_hidden, text, sizeof(text));
    return apps_hidden_parse(text);
}

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .key_cb       = NULL,
};

dialog_t *dialog_apps = &dialog;

static void show_sel(void) {
    for (int i = 0; i < shown_n; i++) {
        bool on = i == sel;

        lv_obj_set_style_border_color(tiles[i], on ? TC(sel_border) : TC(panel_border), 0);
        lv_obj_set_style_border_width(tiles[i], on ? 2 : 1, 0);
        lv_obj_set_style_bg_color(tiles[i], on ? TC(sel_bg) : TC(chip), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tiles[i], 1), on ? TC(sel_text) : TC(text), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(tiles[i], 2), on ? TC(sel_text) : TC(text_dim), 0);
        lv_obj_set_style_img_recolor(lv_obj_get_child(tiles[i], 0), on ? TC(sel_text) : TC(accent), 0);
    }
    lv_obj_scroll_to_view(tiles[sel], LV_ANIM_OFF);
    sel_app = shown[sel];
}

/* Spoken: the app chosen, what it is, and where in the grid; first: the grid's name before it */
static void say_sel(const char *first) {
    int a = shown[sel];

    if (first) {
        voice_say_text_fmt("%s, %s, %s, %i of %i", first, apps[a].name, apps[a].sub, sel + 1, shown_n);
    } else {
        voice_delay_say_text_fmt("%s, %s, %i of %i", apps[a].name, apps[a].sub, sel + 1, shown_n);
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
    lv_async_call(open_async, (void *)(intptr_t)shown[sel]);
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_LEFT:
            if (sel > 0) sel--;
            show_sel();
            say_sel(NULL);
            break;
        case LV_KEY_RIGHT:
            if (sel < shown_n - 1) sel++;
            show_sel();
            say_sel(NULL);
            break;
        case LV_KEY_UP:
            if (sel >= COLS) sel -= COLS;
            show_sel();
            say_sel(NULL);
            break;
        case LV_KEY_DOWN:
            if (sel + COLS < shown_n) sel += COLS;
            show_sel();
            say_sel(NULL);
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

    /* The apps not hidden (Settings > Apps), in the grid's order; the one chosen last time when it is still there */
    uint32_t hidden = apps_menu_hidden();

    shown_n = 0;
    sel     = 0;
    for (int i = 0; i < APPS_N; i++) {
        if (apps_hidden_has(hidden, apps[i].id)) continue;
        if (i == sel_app) sel = shown_n;
        shown[shown_n++] = (uint8_t)i;
    }

    lv_obj_t *box = lv_obj_create(parent);
    dialog.obj    = box;
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, BOX_W, BOX_H(shown_n));
    lv_obj_set_pos(box, (BOARD_DISP_W - BOX_W) / 2, BOX_Y);
    lv_obj_add_style(box, &popup_style, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_label_create(box);
    lv_obj_set_style_text_font(head, &sony_22, 0);
    lv_obj_set_style_text_color(head, TC(text_head), 0);
    lv_label_set_text(head, "APPS");
    lv_obj_set_pos(head, 22, 9);

    lv_obj_t *grid = lv_obj_create(box);  /* the tiles' scrolling view */
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, BOX_W, GRID_H(shown_n));
    lv_obj_set_pos(grid, 0, HEAD_H);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(grid, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(grid, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(grid, TC(text_dim), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);
    lv_obj_set_style_pad_right(grid, 1, LV_PART_SCROLLBAR);

    for (int i = 0; i < shown_n; i++) {
        int       a = shown[i];
        lv_obj_t *t = lv_obj_create(grid);
        tiles[i]    = t;
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, TILE_W, TILE_H);
        lv_obj_set_pos(t, PAD + (i % COLS) * (TILE_W + GAP), (i / COLS) * (TILE_H + GAP));
        lv_obj_set_style_radius(t, 8, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* child 0 the icon (a label stands in when the file is missing), 1 the name, 2 the line */
        lv_obj_t *img = theme_icon_img(t, apps[a].icon, ICON, TC(accent));
        if (!img) {
            img = lv_label_create(t);
            lv_label_set_text(img, "");
        }
        lv_obj_set_pos(img, 10, (TILE_H - ICON) / 2);

        /* The name, then the line in two rows of a smaller font: beside the icon the tile is
         * narrow (117 px) and the lines wrap; with one row's height the second was cut off */
        lv_obj_t *name = lv_label_create(t);
        lv_obj_set_style_text_font(name, &sony_22, 0);
        lv_label_set_text(name, apps[a].name);
        lv_obj_set_pos(name, 10 + ICON + 10, 5);

        lv_obj_t *sub = lv_label_create(t);
        lv_obj_set_style_text_font(sub, &sony_16, 0);
        lv_label_set_text(sub, apps[a].sub);
        lv_obj_set_size(sub, TILE_W - ICON - 30, 36);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(sub, 10 + ICON + 10, 29);
    }

    lv_obj_t *hint = lv_label_create(box);
    lv_obj_set_style_text_font(hint, &sony_20, 0);
    lv_obj_set_style_text_color(hint, TC(text_dim), 0);
    lv_label_set_text(hint, "MFK: choose - press: open - APP or ESC: close");
    lv_obj_set_pos(hint, 22, HEAD_H + GRID_H(shown_n) + 4);

    lv_obj_update_layout(box);  /* the tiles' places, for scrolling to the one chosen */
    show_sel();

    lv_group_add_obj(keyboard_group, box);
    lv_obj_add_event_cb(box, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(box, click_cb, LV_EVENT_CLICKED, NULL);
    voice_focus_quiet(box);
    lv_group_focus_obj(box);
    lv_group_set_editing(keyboard_group, true);
    say_sel("Apps");
}

static void destruct_cb(void) {
    lv_group_remove_obj(dialog.obj);
}

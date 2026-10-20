/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "settings_window.h"

#include "dsp.h"
#include "wifi.h"
#include "btaudio.h"
#include "bt_dev.h"
#include "cfg/settings_manager.h"
#include "cfg/settings_manager_api.h"
#include "factory_reset.h"
#include <vector>
#include <thread>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <functional>

extern "C" {

    #include "app_window.h"
    #include "gui_styles.h"
    #include "theme.h"
    #include "fonts/font_select.h"
    #include "params/params.h"
    #include "screen_light.h"
    #include "radio.h"
    #include "gui_events.h"
    #include "key_input.h"
    #include "paddle_keyer.h"
    #include "clock.h"
    #include "bat_est.h"
    #include "audio.h"
    #include "smeter.h"
    #include "main.h"
    #include "msg_box.h"
    #include "text_popup.h"
    #include "station.h"
    #include "tci_server.h"
    #include "cluster/client.h"
    #include "cluster/spots.h"
    #include "broadcast_db.h"

    #include "lvgl/lvgl.h"
    #include <sys/time.h>
    #include <time.h>
    #include <fcntl.h>
    #include <stdlib.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
    #include <linux/rtc.h>
    #include <linux/input.h>
    #include <errno.h>
    #include <sys/stat.h>

}

#define ROW_H       44  /* option row and control height */
#define UPD_ROW_H   36  /* Info page: the Update button and its status line */
#define UPD_NOTE_H  20
/* Settings text two sizes over the sony_N it names (readability); VISION already scales it up */
#define SF(n)       font_down(&sony_##n, theme_shape.vision ? 0 : -2)

/* Right of the row label: six columns of 57 px, 5 px apart. SMALL_n spans n of them. */
#define SMALL_PAD   5
#define SMALL_1     57
#define SMALL_N(n)  (SMALL_1 * (n) + SMALL_PAD * ((n) - 1))
#define SMALL_2     SMALL_N(2)
#define SMALL_3     SMALL_N(3)
#define SMALL_4     SMALL_N(4)
#define SMALL_6     SMALL_N(6)

static void make_general_page();
static void make_display_page();
static void make_audio_page();
static void make_wireless_page();
static void make_bluetooth_page();
static void eth_draft_load();
static void make_info_page();
static void make_keys_page();
static void make_cluster_page();
static void make_txcal_page();
static void make_walkie_page();
static void wl_row_label(uint8_t row, const char *text);

static lv_obj_t     *grid;
static lv_obj_t     *page;      /* scrolling column of cards */
static lv_coord_t   col_dsc[] = { LV_GRID_FR(1), SMALL_1, SMALL_1, SMALL_1, SMALL_1, SMALL_1, SMALL_1, LV_GRID_TEMPLATE_LAST };
static lv_coord_t   col_station[] = { LV_GRID_FR(1), 150, LV_GRID_TEMPLATE_LAST };
static lv_coord_t   col_datetime[] = { LV_GRID_FR(1), 78, 78, 100, LV_GRID_TEMPLATE_LAST };
static lv_coord_t   row_pool[8][64];    /* row descriptors: one per card of the page */
static lv_coord_t   *row_dsc = row_pool[0];
static int          card_n;

/* Sliders that follow a setting changed elsewhere (band change) */
static std::vector<Subscription> observers;

/* Tabs: three pages of buttons, the first button of each turns the page. A tab's button (or its
 * line on the rail) builds the tab's page. */

static void tab_press(button_data_t *btn);
static void construct_cb(lv_obj_t *parent);
static void destruct_cb();
static void key_cb(lv_event_t *e);

static button_data_t btn_p1        = { .type = BTN_TEXT, .label = "(SETTINGS\n1:3)", .press = button_next_page_cb };
static button_data_t btn_p2        = { .type = BTN_TEXT, .label = "(SETTINGS\n2:3)", .press = button_next_page_cb };
static button_data_t btn_p3        = { .type = BTN_TEXT, .label = "(SETTINGS\n3:3)", .press = button_next_page_cb };
static button_data_t btn_general   = { .type = BTN_TEXT, .label = "General", .press = tab_press };
static button_data_t btn_display   = { .type = BTN_TEXT, .label = "Display", .press = tab_press };
static button_data_t btn_audio     = { .type = BTN_TEXT, .label = "Audio", .press = tab_press };
static button_data_t btn_wireless  = { .type = BTN_TEXT, .label = "Wireless", .press = tab_press };
static button_data_t btn_bluetooth = { .type = BTN_TEXT, .label = "Bluetooth", .press = tab_press };
static button_data_t btn_keys      = { .type = BTN_TEXT, .label = "Keys", .press = tab_press };
static button_data_t btn_cluster   = { .type = BTN_TEXT, .label = "Cluster", .press = tab_press };
static button_data_t btn_txcal     = { .type = BTN_TEXT, .label = "TX Cal", .press = tab_press };
static button_data_t btn_info      = { .type = BTN_TEXT, .label = "Info", .press = tab_press };
static button_data_t btn_walkie    = { .type = BTN_TEXT, .label = "Walkie", .press = tab_press };

static buttons_page_t btn_page   = {{&btn_p1, &btn_general, &btn_display, &btn_audio, &btn_wireless}};
static buttons_page_t btn_page_2 = {{&btn_p2, &btn_bluetooth, &btn_keys, &btn_cluster, &btn_txcal}};
static buttons_page_t btn_page_3 = {{&btn_p3, &btn_info, &btn_walkie}};
static buttons_page_t *btn_pages[] = {&btn_page, &btn_page_2, &btn_page_3};

static dialog_t dialog = {
    .construct_cb = construct_cb, .destruct_cb = destruct_cb, .audio_cb = NULL, .btn_page = &btn_page,
    .key_cb = key_cb, .run = false,
};

dialog_t *dialog_settings = &dialog;

/* The Ethernet card starts from the settings in use, not from a draft left on the tab */
static void make_wireless_tab() {
    eth_draft_load();
    make_wireless_page();
}

/* In the order of the buttons, four to a page */
static const struct {
    button_data_t *btn;
    void         (*make)();
    const char    *icon;    /* on the rail with icon menus (/usr/share/x6100/icons) */
} tabs[] = {
    {&btn_general, make_general_page, "general"},   {&btn_display, make_display_page, "display"},
    {&btn_audio, make_audio_page, "audio"},         {&btn_wireless, make_wireless_tab, "wireless"},
    {&btn_bluetooth, make_bluetooth_page, "bluetooth"}, {&btn_keys, make_keys_page, "keys"},
    {&btn_cluster, make_cluster_page, "cluster"},   {&btn_txcal, make_txcal_page, "txcal"},
    {&btn_info, make_info_page, "info"},            {&btn_walkie, make_walkie_page, "walkie"},
};

/* Left rail: the tabs of all button pages, numbered, the current one marked */

#define RAIL_W      150
#define RAIL_N      (int)(sizeof(tabs) / sizeof(tabs[0]))

static lv_obj_t      *rail_items[RAIL_N];

static void rail_click_cb(lv_event_t *e) {
    int             i  = (int)(intptr_t)lv_event_get_user_data(e);
    buttons_page_t *pg = btn_pages[i / 4];

    if (buttons_get_cur_page() != pg) {
        buttons_unload_page();
        buttons_load_page(pg);
    }
    tab_press(tabs[i].btn);
}

static void rail_create() {
    lv_obj_t  *rail = lv_obj_create(dialog.obj);
    lv_coord_t h    = (DIALOG_H - 8) / RAIL_N;

    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, RAIL_W, DIALOG_H - 8);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_color(rail, TC(dialog_border), 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < RAIL_N; i++) {
        lv_obj_t *item = lv_obj_create(rail);
        char      name[16];
        int       k;

        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, RAIL_W - 4, h);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(item, TC(sel_bg), 0);
        lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(item, TC(accent), 0);
        lv_obj_add_event_cb(item, rail_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* child 0: the tab's icon (icon menus), else its number */
        lv_obj_t *num = params.icon_menus.x ? theme_icon_img(item, tabs[i].icon, 24, TC(text_dim)) : NULL;
        if (num) {
            lv_obj_align(num, LV_ALIGN_LEFT_MID, 7, 0);
        } else {
            num = lv_label_create(item);
            lv_obj_set_style_text_font(num, SF(14), 0);
            lv_label_set_text_fmt(num, "%02d", i + 1);
            lv_obj_align(num, LV_ALIGN_LEFT_MID, 10, 0);
        }

        for (k = 0; tabs[i].btn->label[k] && k < 15; k++) name[k] = toupper(tabs[i].btn->label[k]);
        name[k] = 0;
        lv_obj_t *lbl = lv_label_create(item);
        /* BLUETOOTH inside the rail: a size down, two in VISION */
        const lv_font_t *fonts[] = {SF(18), SF(16), SF(14)};
        lv_point_t       size;
        int              f = 0;

        do {
            lv_txt_get_size(&size, name, fonts[f], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        } while (size.x > RAIL_W - 4 - 38 - 2 && ++f < 2);
        lv_obj_set_style_text_font(lbl, fonts[f], 0);
        lv_label_set_text(lbl, name);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 38, 0);
        rail_items[i] = item;
    }
}

static void rail_mark(button_data_t *btn_data) {
    for (int i = 0; i < RAIL_N; i++) {
        lv_obj_t *item = rail_items[i];
        bool      on   = tabs[i].btn == btn_data;

        lv_obj_set_style_bg_opa(item, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, on ? 4 : 0, 0);
        lv_obj_t *mark = lv_obj_get_child(item, 0);
        if (lv_obj_check_type(mark, &lv_img_class))
            lv_obj_set_style_img_recolor(mark, on ? TC(sel_text) : TC(text_dim), 0);
        else
            lv_obj_set_style_text_color(mark, on ? TC(sel_text) : TC(text_faint), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(item, 1), on ? TC(sel_text) : TC(text_dim), 0);
    }
}

static void mark_tab(button_data_t *btn_data) {
    for (buttons_page_t *pg : btn_pages) {
        for (button_data_t *btn : pg->items) {
            buttons_mark(btn, false);
        }
    }
    buttons_mark(btn_data, true);
    rail_mark(btn_data);
}

static void tab_press(button_data_t *btn) {
    for (auto &t : tabs) {
        if (t.btn == btn) t.make();
    }
    mark_tab(btn);
}

/* Controls tied to a setting: the event's user data is the setting */

static void bool_param_cb(lv_event_t *e) {
    params_bool_set((params_bool_t *)lv_event_get_user_data(e), lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void int_param_cb(lv_event_t *e) {
    ((ParamInt *)lv_event_get_user_data(e))->set(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void uint8_choice_cb(lv_event_t *e) {
    params_uint8_set((params_uint8_t *)lv_event_get_user_data(e), lv_dropdown_get_selected(lv_event_get_target(e)));
}

static void int_choice_cb(lv_event_t *e) {
    ((ParamInt *)lv_event_get_user_data(e))->set(lv_dropdown_get_selected(lv_event_get_target(e)));
}

/* Themes and fonts are applied live: every window takes its colours and fonts when
 * built, so the main screen is built again (main_screen_set_theme()) */
static lv_obj_t *theme_dd;
static lv_obj_t *font_dd;

/* Settings goes with the rest of the screen, then opens again on Display, on the
 * dropdown that was changed (arg) */
static void theme_apply(void *arg) {
    bool font = arg != NULL;

    main_screen_set_theme(params.theme_name.x);
    main_screen_start_app(ACTION_APP_SETTINGS);
    tab_press(&btn_display);
    lv_group_focus_obj(font ? font_dd : theme_dd);
}

static void font_update_cb(lv_event_t * e) {
    uint8_choice_cb(e);
    if (theme_shape.font >= 0) {
        msg_update_text_fmt("Font: this theme has its own");   /* RS, Black Metal, Total Black */
        return;
    }
    params_flush();
    lv_async_call(theme_apply, (void *)1);   /* not from inside the dropdown's own event */
}

static void theme_update_cb(lv_event_t * e) {
    char name[32];

    lv_dropdown_get_selected_str(lv_event_get_target(e), name, sizeof(name));
    params_str_set(&params.theme_name, name);
    params_flush();
    cfg_api_flush_all();
    lv_async_call(theme_apply, NULL);   /* not from inside the dropdown's own event */
}

/* Row pieces. A row has its label in column 0 (wl_row_label()) and its controls in the six
 * columns at the right; card_end() moves a lone control to the right edge and flattens them. */

/* A frameless box over `span` columns, the control centred in it */
static lv_obj_t * row_cell(uint8_t row, uint8_t col, uint8_t span, lv_coord_t w) {
    lv_obj_t *cell = lv_obj_create(grid);

    lv_obj_set_size(cell, w, ROW_H);
    lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_START, col, span, LV_GRID_ALIGN_CENTER, row, 1);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(cell);
    return cell;
}

static lv_obj_t * switch_in(lv_obj_t *cell, bool on, lv_event_cb_t cb, void *user) {
    lv_obj_t *sw = lv_switch_create(cell);

    dialog_item(&dialog, sw);
    lv_obj_center(sw);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static lv_obj_t * switch_bool(lv_obj_t *cell, params_bool_t *p) {
    return switch_in(cell, p->x, bool_param_cb, p);
}

/* A switch in a cell of its own, 15 px in from each side */
static lv_obj_t * row_switch(uint8_t row, uint8_t col, uint8_t span, lv_coord_t w, bool on, lv_event_cb_t cb, void *user) {
    lv_obj_t *sw = switch_in(row_cell(row, col, span, w), on, cb, user);

    lv_obj_set_width(sw, w - 30);
    return sw;
}

static lv_obj_t * bool_switch(uint8_t row, uint8_t col, uint8_t span, lv_coord_t w, params_bool_t *p) {
    return row_switch(row, col, span, w, p->x, bool_param_cb, p);
}

static lv_obj_t * param_switch(uint8_t row, uint8_t col, uint8_t span, lv_coord_t w, ParamInt &p) {
    return row_switch(row, col, span, w, p.get(), int_param_cb, &p);
}

/* A row of its own: the label and a switch */
static uint8_t make_param_switch(uint8_t row, const char *label, ParamInt &param) {
    wl_row_label(row, label);
    param_switch(row, 4, 3, SMALL_3, param);
    return row + 1;
}

static void slider_vtext(lv_obj_t *slider, const char *fmt, va_list ap) {
    char s[32];

    lv_vsnprintf(s, sizeof(s), fmt, ap);
    lv_label_set_text((lv_obj_t *)lv_obj_get_user_data(slider), s);
}

/* The value at the right of a slider */
static void slider_text(lv_obj_t *slider, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    slider_vtext(slider, fmt, ap);
    va_end(ap);
}

/* A slider at the left of its cell counting steps from lo to hi, the value as text at the right
 * (the label is the slider's user data) */
static lv_obj_t * row_slider(lv_obj_t *cell, int32_t pos, int32_t lo, int32_t hi, lv_coord_t w,
                             lv_event_cb_t cb, void *user, const char *fmt, ...) {
    lv_obj_t *s = lv_slider_create(cell);
    lv_obj_t *label;
    va_list   ap;

    dialog_item(&dialog, s);
    lv_slider_set_mode(s, LV_SLIDER_MODE_NORMAL);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, pos, LV_ANIM_OFF);
    lv_obj_set_width(s, w);
    lv_obj_align(s, LV_ALIGN_LEFT_MID, 0, 0);   /* in the middle of the box, as its value */

    label = lv_label_create(cell);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, 12, 0);
    lv_obj_set_style_text_color(label, TC(text), 0);
    lv_obj_set_user_data(s, label);
    va_start(ap, fmt);
    slider_vtext(s, fmt, ap);
    va_end(ap);

    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, user);
    return s;
}

/* Sliders over a float setting in fixed steps, the value as 0.0 */
typedef struct {
    float  step;
    void (*set)(float v);
} float_slider_t;

static void float_slider_cb(lv_event_t *e) {
    lv_obj_t       *s = lv_event_get_target(e);
    float_slider_t *f = (float_slider_t *)lv_event_get_user_data(e);
    float           v = lv_slider_get_value(s) * f->step;

    slider_text(s, "%0.1f", v);
    f->set(v);
}

static lv_obj_t * float_slider(lv_obj_t *cell, float val, float lo, float hi, lv_coord_t w, float_slider_t *f) {
    return row_slider(cell, val / f->step, lo / f->step, hi / f->step, w, float_slider_cb, f, "%0.1f", val);
}

/* A spinbox of `digits` digits */
static lv_obj_t * row_spin(uint8_t row, uint8_t col, uint8_t span, lv_coord_t w, int32_t val, int32_t lo, int32_t hi,
                           uint8_t digits) {
    lv_obj_t *sb = lv_spinbox_create(grid);

    dialog_item(&dialog, sb);
    lv_spinbox_set_value(sb, val);
    lv_spinbox_set_range(sb, lo, hi);
    lv_spinbox_set_digit_format(sb, digits, 0);
    lv_spinbox_set_digit_step_direction(sb, LV_DIR_LEFT);
    lv_obj_set_size(sb, w, ROW_H);
    lv_obj_set_grid_cell(sb, LV_GRID_ALIGN_START, col, span, LV_GRID_ALIGN_CENTER, row, 1);
    return sb;
}

/* A dropdown, `sel` chosen */
static lv_obj_t * row_choice(uint8_t row, uint8_t col, uint8_t span, lv_coord_t w, const char *options, uint16_t sel,
                             lv_event_cb_t cb, void *user) {
    lv_obj_t *dd = lv_dropdown_create(grid);

    dialog_item(&dialog, dd);
    styles_dropdown_list(lv_dropdown_get_list(dd));
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_symbol(dd, NULL);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, user);
    lv_obj_set_size(dd, w, ROW_H);
    lv_obj_set_grid_cell(dd, LV_GRID_ALIGN_START, col, span, LV_GRID_ALIGN_CENTER, row, 1);
    return dd;
}

/* A row of its own: the label and a dropdown over three columns */
static uint8_t make_param_choice(uint8_t row, const char *label, const char *options, ParamInt &param) {
    wl_row_label(row, label);
    row_choice(row, 1, 3, SMALL_3, options, param.get(), int_choice_cb, &param);
    return row + 1;
}

/* Spectrum and waterfall looks: while one of them is changed the window fades to 60% and the
 * main screen shows through. Not with a theme's window image (RS): faded, it turns see-through. */
static void fade_exec(void *obj, int32_t opa) {
    lv_obj_set_style_bg_img_opa((lv_obj_t *)obj, opa, 0);
}

static void preview_fade_cb(lv_event_t *e) {
    lv_obj_t *obj  = lv_event_get_target(e);
    bool      edit = (lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_spinbox_class)) &&
                     lv_obj_has_state(obj, LV_STATE_EDITED);
    bool      sw   = lv_obj_check_type(obj, &lv_switch_class) && lv_event_get_code(e) == LV_EVENT_FOCUSED;
    lv_opa_t  from = lv_obj_get_style_bg_img_opa(dialog.obj, 0);
    lv_opa_t  to   = edit || sw ? LV_OPA_60 : LV_OPA_COVER;
    lv_anim_t a;

    if (lv_obj_get_style_bg_img_src(dialog.obj, 0) || from == to) {
        return;
    }
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, fade_exec);
    lv_anim_set_var(&a, dialog.obj);
    lv_anim_set_time(&a, 500);
    lv_anim_set_values(&a, from, to);
    lv_anim_start(&a);
}

static void preview_fade(lv_obj_t *obj) {
    lv_obj_add_event_cb(obj, preview_fade_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(obj, preview_fade_cb, LV_EVENT_DEFOCUSED, NULL);
}

/* Date and time, local. A change sets the system clock at once, the RTC (in UTC) when the focus
 * comes back to a field after the edit. */

static struct tm dt_tm;         /* the fields */
static struct tm dt_utc;        /* the same instant, for the RTC */
static bool      dt_changed;
static bool      zone_refresh;  /* spinboxes being reloaded: not a user edit */
static lv_obj_t *dt_spin[6];

/* Day, month, year, hour, min, sec: shown as the struct tm field + base */
static const struct {
    int        base;
    int        lo, hi;
    uint8_t    digits;
    lv_coord_t w;
} dt_spec[6] = {
    {0, 1, 31, 2, 78}, {1, 1, 12, 2, 78}, {1900, 2020, 2038, 4, 100}, {0, 0, 23, 2, 78}, {0, 0, 59, 2, 78}, {0, 0, 59, 2, 78},
};

static int * dt_field(int i) {
    int *f[6] = {&dt_tm.tm_mday, &dt_tm.tm_mon, &dt_tm.tm_year, &dt_tm.tm_hour, &dt_tm.tm_min, &dt_tm.tm_sec};

    return f[i];
}

/* The fields again from dt_tm */
static void dt_show() {
    zone_refresh = true;
    for (int i = 0; i < 6; i++) {
        lv_spinbox_set_value(dt_spin[i], *dt_field(i) + dt_spec[i].base);
    }
    zone_refresh = false;
}

static void dt_changed_cb(lv_event_t *e) {
    struct timespec tp = {};

    if (zone_refresh) return;
    for (int i = 0; i < 6; i++) {
        *dt_field(i) = lv_spinbox_get_value(dt_spin[i]) - dt_spec[i].base;
    }
    dt_changed = true;

    dt_tm.tm_isdst = -1;
    tp.tv_sec = mktime(&dt_tm);
    if (clock_settime(CLOCK_REALTIME, &tp) != 0) {
        LV_LOG_ERROR("Can't set system time: %s\n", strerror(errno));
        return;
    }
    gmtime_r(&tp.tv_sec, &dt_utc);
}

static void dt_rtc_cb(lv_event_t *e) {
    if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_EDITED) || !dt_changed) {
        return;
    }
    int rtc = open("/dev/rtc1", O_WRONLY);

    if (rtc <= 0) {
        LV_LOG_ERROR("Can't open /dev/rtc1: %s\n", strerror(errno));
    } else {
        int res = ioctl(rtc, RTC_SET_TIME, &dt_utc);

        close(rtc);
        if (res != 0) {
            LV_LOG_ERROR("Can't set RTC time: %s\n", strerror(errno));
            return;     /* tried again at the next focus */
        }
    }
    dt_changed = false;
}

/* Date (fields 0-2) or time (3-5) */
static uint8_t make_dt_row(uint8_t row, const char *label, int first) {
    wl_row_label(row, label);
    for (int i = first; i < first + 3; i++) {
        dt_spin[i] = row_spin(row, 1 + i - first, 1, dt_spec[i].w, *dt_field(i) + dt_spec[i].base, dt_spec[i].lo,
                              dt_spec[i].hi, dt_spec[i].digits);
        lv_obj_add_event_cb(dt_spin[i], dt_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(dt_spin[i], dt_rtc_cb, LV_EVENT_FOCUSED, NULL);
    }
    return row + 1;
}

/* Backlight: the timeout, the level in use and the dimmed one (a range slider), the key lights */

static void bl_timeout_cb(lv_event_t *e) {
    params_lock();
    params.brightness_timeout = lv_spinbox_get_value(lv_event_get_target(e));
    params_unlock(&params.dirty.brightness_timeout);
    backlight_tick();
}

static void bl_level_cb(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);

    params_lock();
    params.brightness_normal = lv_slider_get_value(s);
    params_unlock(&params.dirty.brightness_normal);
    params_lock();
    params.brightness_idle = lv_slider_get_left_value(s);
    params_unlock(&params.dirty.brightness_idle);
    backlight_set_brightness(params.brightness_normal);
}

static void bl_keys_cb(lv_event_t *e) {
    backlight_set_buttons((buttons_light_t)lv_dropdown_get_selected(lv_event_get_target(e)));
}

static uint8_t make_backlight(uint8_t row) {
    lv_obj_t *obj;

    wl_row_label(row, "Timeout, Brightness");
    obj = row_spin(row, 1, 2, SMALL_2, params.brightness_timeout, 5, 120, 3);
    lv_obj_add_event_cb(obj, bl_timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);

    obj = lv_slider_create(row_cell(row, 3, 4, SMALL_4));
    dialog_item(&dialog, obj);
    lv_slider_set_mode(obj, LV_SLIDER_MODE_RANGE);
    /* The levels go in before the range: an idle level of -1 shows as 0 */
    lv_slider_set_value(obj, params.brightness_normal, LV_ANIM_OFF);
    lv_slider_set_left_value(obj, params.brightness_idle, LV_ANIM_OFF);
    lv_slider_set_range(obj, -1, 9);
    lv_obj_set_width(obj, SMALL_4 - 30);
    lv_obj_center(obj);
    lv_obj_add_event_cb(obj, bl_level_cb, LV_EVENT_VALUE_CHANGED, NULL);
    row++;

    wl_row_label(row, "Buttons brightness");
    row_choice(row, 1, 6, SMALL_6, " Always Off \n Always On \n Temporarily On ", params.brightness_buttons, bl_keys_cb, NULL);
    return row + 1;
}

/* Line-in and Line-out: the codec's gains (user data 0: in, else out) */
static void line_gain_cb(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);
    int32_t   v = lv_slider_get_value(s);

    if (lv_event_get_user_data(e)) {
        radio_set_line_out(v);
    } else {
        radio_set_line_in(v);
    }
    slider_text(s, "%d", v);
}

static uint8_t make_line_gain(uint8_t row) {
    wl_row_label(row, "Line-in, Line-out");
    row_slider(row_cell(row, 1, 3, SMALL_3), params.line_in, 0, 36, SMALL_3 - 90, line_gain_cb, NULL, "%d", params.line_in);
    row_slider(row_cell(row, 4, 3, SMALL_3), params.line_out, 0, 36, SMALL_3 - 90, line_gain_cb, (void *)1, "%d", params.line_out);
    return row + 1;
}

/* Play and Rec gain in dB: what the mixer took is saved and goes to the Line-in too */
static void play_rec_gain_cb(lv_event_t *e) {
    lv_obj_t *s   = lv_event_get_target(e);
    bool      rec = lv_event_get_user_data(e) != NULL;
    float     db  = rec ? audio_set_rec_vol(lv_slider_get_value(s)) : audio_set_play_vol(lv_slider_get_value(s));

    params_float_set(rec ? &params.rec_gain_db_f : &params.play_gain_db_f, db);
    radio_set_line_in(db);
    slider_text(s, "%.1f", db);
}

static uint8_t make_audio_gain(uint8_t row) {
    float play = params.play_gain_db_f.x;
    float rec  = params.rec_gain_db_f.x;

    wl_row_label(row, "Play, Rec gain");
    row_slider(row_cell(row, 1, 3, SMALL_3), play, -10, 10, SMALL_3 - 95, play_rec_gain_cb, NULL, "%.1f", play);
    row_slider(row_cell(row, 4, 3, SMALL_3), rec, -10, 10, SMALL_3 - 95, play_rec_gain_cb, (void *)1, "%.1f", rec);
    return row + 1;
}

/* Popups on the main screen: frequency, info, ALC */
static uint8_t make_mag(uint8_t row) {
    params_bool_t *p[] = {&params.mag_freq, &params.mag_info, &params.mag_alc};

    wl_row_label(row, "Popups Freq/Info/ALC");
    for (int i = 0; i < 3; i++) {
        bool_switch(row, 1 + 2 * i, 3, SMALL_2, p[i]);
    }
    return row + 1;
}

/* What the front keys do when held, what the hand mic keys do */

typedef struct {
    const char     *label;
    press_action_t  action;
} key_action_t;

static const key_action_t front_actions[] = {
    {" None ", ACTION_NONE},
    {" Screenshot ", ACTION_SCREENSHOT},
    {" RX recorder on/off ", ACTION_RECORDER},
    {" Screen rec on/off ", ACTION_SCREEN_REC},
    {" Mute ", ACTION_MUTE},
    {" WF mode ", ACTION_WF_MODE},
    {" ATT on/off ", ACTION_ATT_TOGGLE},
    {" PRE on/off ", ACTION_PRE_TOGGLE},
    {" Tuner on/off ", ACTION_ATU_TOGGLE},
    {" Tuner: tune ", ACTION_ATU_TUNE},
    /* Digital modes, in the order of the Modes menu */
    {" APP RTTY ", ACTION_APP_RTTY},
    {" APP FT8 ", ACTION_APP_FT8},
    {" APP WSPR ", ACTION_APP_WSPR},
    {" APP JS8 ", ACTION_APP_JS8},
    {" APP SSTV ", ACTION_APP_SSTV},
    {" APP WeFax ", ACTION_APP_WEFAX},
    {" APP NavTex ", ACTION_APP_NAVTEX},
    /* Tools */
    {" APP Cluster ", ACTION_APP_CLUSTER},
    {" APP CW Trainer ", ACTION_APP_CWTRAINER},
    {" APP Field Ops ", ACTION_APP_FIELD},
    {" APP Bands ", ACTION_APP_BANDS},
    {" APP Channels ", ACTION_APP_CHANNELS},
    {" APP RX recorder ", ACTION_APP_RECORDER},
    {" APP SWR Scan ", ACTION_APP_SWRSCAN},
    {" APP GPS ", ACTION_APP_GPS},
    {" APP QTH grid ", ACTION_APP_QTH},
    {" APP Settings ", ACTION_APP_SETTINGS},
};

static const key_action_t mic_actions[] = {
    {" None ", ACTION_NONE},
    {" RX recorder on/off ", ACTION_RECORDER},
    {" Screen rec on/off ", ACTION_SCREEN_REC},
    {" Mute ", ACTION_MUTE},
    {" Step up ", ACTION_STEP_UP},
    {" Step down ", ACTION_STEP_DOWN},
    {" NR toggle ", ACTION_NR_TOGGLE},
    {" NB toggle ", ACTION_NB_TOGGLE},
    {" WF mode ", ACTION_WF_MODE},
    {" ATT on/off ", ACTION_ATT_TOGGLE},
    {" PRE on/off ", ACTION_PRE_TOGGLE},
    {" Tuner on/off ", ACTION_ATU_TOGGLE},
    {" Tuner: tune ", ACTION_ATU_TUNE},
};

#define KEY_ROWS_FRONT  6   /* then the hand mic's */

typedef struct {
    const char *label;
    uint8_t    *val;
    bool       *dirty;
} key_row_t;

static const key_row_t key_rows[] = {
    {"GEN", &params.long_gen, &params.dirty.long_gen},
    {"APP", &params.long_app, &params.dirty.long_app},
    {"KEY", &params.long_key, &params.dirty.long_key},
    {"MSG", &params.long_msg, &params.dirty.long_msg},
    {"DFN", &params.long_dfn, &params.dirty.long_dfn},
    {"DFL", &params.long_dfl, &params.dirty.long_dfl},
    {"F1", &params.press_f1, &params.dirty.press_f1},
    {"F2", &params.press_f2, &params.dirty.press_f2},
    {"F1 long", &params.long_f1, &params.dirty.long_f1},
    {"F2 long", &params.long_f2, &params.dirty.long_f2},
};

static const key_action_t * key_actions(const key_row_t *k, size_t *n) {
    if (k < key_rows + KEY_ROWS_FRONT) {
        *n = sizeof(front_actions) / sizeof(front_actions[0]);
        return front_actions;
    }
    *n = sizeof(mic_actions) / sizeof(mic_actions[0]);
    return mic_actions;
}

static void key_action_cb(lv_event_t *e) {
    const key_row_t *k = (const key_row_t *)lv_event_get_user_data(e);
    size_t           n;
    press_action_t   action = key_actions(k, &n)[lv_dropdown_get_selected(lv_event_get_target(e))].action;

    params_lock();
    *k->val = action;
    params_unlock(k->dirty);
}

/* key_rows from `first` to `last` (exclusive) */
static uint8_t make_key_rows(uint8_t row, int first, int last) {
    for (int i = first; i < last; i++) {
        const key_row_t    *k = &key_rows[i];
        size_t              n;
        const key_action_t *list = key_actions(k, &n);
        std::string         options;
        uint16_t            sel = 0;

        for (size_t a = 0; a < n; a++) {
            options += std::string(a ? "\n" : "") + list[a].label;
            if (list[a].action == *k->val) sel = a;
        }
        wl_row_label(row, k->label);
        row_choice(row++, 1, 6, SMALL_6, options.c_str(), sel, key_action_cb, (void *)k);
    }
    return row;
}

/* Transverters: the band they cover (from, to) and the shift, in MHz */
static void xvrt_cb(lv_event_t *e) {
    ((ParamInt *)lv_event_get_user_data(e))->set(lv_spinbox_get_value(lv_event_get_target(e)) * 1000000L);
}

static uint8_t make_transverters(uint8_t row) {
    ParamInt *p[2][3] = {
        {&cfg_sm.p_transverter_0_from, &cfg_sm.p_transverter_0_to, &cfg_sm.p_transverter_0_shift},
        {&cfg_sm.p_transverter_1_from, &cfg_sm.p_transverter_1_to, &cfg_sm.p_transverter_1_shift},
    };
    const int lo[3] = {70, 70, 42};

    for (auto &x : p) {
        char label[24];

        snprintf(label, sizeof(label), "%i: from, to, shift", x[0]->context_id() + 1);
        wl_row_label(row, label);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *sb = row_spin(row, 1 + 2 * i, 2, SMALL_2, x[i]->get() / 1000000L, lo[i], 500, 3);

            lv_obj_add_event_cb(sb, xvrt_cb, LV_EVENT_VALUE_CHANGED, x[i]);
        }
        row++;
    }
    return row;
}

/* Spectrum above the waterfall on/off; the waterfall takes its place */

/* The main screen follows it live (main_screen.c); the page again, with or without the spectrum
 * rows, after the event: the switch that got it is deleted */
static lv_obj_t *spectrum_show_sw;

static void spectrum_show_page(void *arg) {
    if (!page) {
        return;
    }
    make_display_page();
    lv_group_focus_obj(spectrum_show_sw);
}

static void spectrum_show_cb(lv_event_t *e) {
    lv_async_call(spectrum_show_page, NULL);
}

static uint8_t make_spectrum_show(uint8_t row) {
    wl_row_label(row, "Spectrum");
    spectrum_show_sw = param_switch(row, 1, 3, SMALL_3, cfg_sm.p_spectrum_show);
    lv_obj_add_event_cb(spectrum_show_sw, spectrum_show_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return row + 1;
}

/* Spectrum fill, peaks */
static uint8_t make_spectrum_fill_peak(uint8_t row) {
    wl_row_label(row, "Fill, peaks");
    preview_fade(bool_switch(row, 1, 3, SMALL_3, &params.spectrum_filled));
    preview_fade(bool_switch(row, 4, 3, SMALL_3, &params.spectrum_peak));
    return row + 1;
}

/* Sliders over an int setting: the slider counts steps, the label shows the value */
typedef struct {
    ParamInt   *param;
    int32_t     min;
    int32_t     max;
    int32_t     step;
    const char *fmt;
} param_slider_t;

static void param_slider_cb(lv_event_t *e) {
    lv_obj_t       *obj = lv_event_get_target(e);
    param_slider_t *p   = (param_slider_t *)lv_event_get_user_data(e);
    int32_t         val = lv_slider_get_value(obj) * p->step;

    lv_label_set_text_fmt((lv_obj_t *)lv_obj_get_user_data(obj), p->fmt, val);
    p->param->set(val);
}

static lv_obj_t * param_slider(lv_obj_t *cell, lv_coord_t w, param_slider_t *p) {
    int32_t val = p->param->get();

    return row_slider(cell, val / p->step, p->min / p->step, p->max / p->step, w, param_slider_cb, p, p->fmt, val);
}

/* Waterfall and spectrum levels: automatic with an offset, or a fixed min and max */

static float_slider_t auto_offset_fs = {0.5f, [](float v) { cfg_sm.p_auto_level_offset.set(v); }};
static param_slider_t grid_min_slider = {&cfg_sm.p_band_grid_min, S_MIN, S7, 1, "%d"};
static param_slider_t grid_max_slider = {&cfg_sm.p_band_grid_max, S8, S9_40, 1, "%d"};

static uint8_t make_auto_offset(uint8_t row) {
    wl_row_label(row, "Auto level, offset");
    param_switch(row, 1, 3, SMALL_3, cfg_sm.p_auto_level_enabled);
    preview_fade(float_slider(row_cell(row, 4, 3, SMALL_3), cfg_sm.p_auto_level_offset.get(), -15.0f, 15.0f,
                              SMALL_3 - 120, &auto_offset_fs));
    return row + 1;
}

static uint8_t make_grid_min_max(uint8_t row) {
    wl_row_label(row, "Manual min, max");
    preview_fade(param_slider(row_cell(row, 1, 3, SMALL_3), SMALL_3 - 120, &grid_min_slider));
    preview_fade(param_slider(row_cell(row, 4, 3, SMALL_3), SMALL_3 - 120, &grid_max_slider));
    return row + 1;
}

/* Waterfall centre line and zoom */
static uint8_t make_waterfall_line_zoom(uint8_t row) {
    wl_row_label(row, "Line, zoom");
    preview_fade(bool_switch(row, 1, 3, SMALL_3, &params.waterfall_center_line));
    bool_switch(row, 4, 3, SMALL_3, &params.waterfall_zoom);
    return row + 1;
}

/* Speaker mode: the switch is lit for spmode 0, a change steps the mode */
static void sp_mode_cb(lv_event_t *e) {
    radio_change_spmode(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? -1 : 1);
}

static uint8_t make_sp_mode(uint8_t row) {
    wl_row_label(row, "Speaker mode");
    row_switch(row, 4, 3, SMALL_3, !params.spmode.x, sp_mode_cb, NULL);
    return row + 1;
}

/* TX audio: compressor threshold and makeup, the TX filter */

static float_slider_t comp_th_fs     = {0.5f, [](float v) { cfg_sm.p_comp_threshold_offset.set(v); }};
static float_slider_t comp_mk_fs     = {0.5f, [](float v) { cfg_sm.p_comp_makeup_offset.set(v); }};
static param_slider_t tx_low_slider  = {&cfg_sm.p_tx_filter_low, 50, 400, 10, "%d"};
static param_slider_t tx_high_slider = {&cfg_sm.p_tx_filter_high, 2000, 4000, 10, "%d"};

static uint8_t make_comp(uint8_t row) {
    wl_row_label(row, "Compressor, makeup");
    float_slider(row_cell(row, 1, 3, SMALL_3), cfg_sm.p_comp_threshold_offset.get(), -15.0f, 15.0f, SMALL_3 - 120,
                 &comp_th_fs);
    float_slider(row_cell(row, 4, 3, SMALL_3), cfg_sm.p_comp_makeup_offset.get(), -15.0f, 15.0f, SMALL_3 - 120,
                 &comp_mk_fs);
    return row + 1;
}

static uint8_t make_tx_filter(uint8_t row) {
    wl_row_label(row, "Filter low");
    param_slider(row_cell(row, 1, 3, SMALL_6), SMALL_6 - 150, &tx_low_slider);
    row++;
    wl_row_label(row, "Filter high");
    param_slider(row_cell(row, 1, 3, SMALL_6), SMALL_6 - 150, &tx_high_slider);
    return row + 1;
}

/* TX calibration, per band: the IQ offsets (in steps of 50), the band output gain; the codec gain */

#define TX_OFFSET_SCALE     50
#define OUTPUT_GAIN_STEP    0.2f

static float_slider_t dac_gain_fs   = {OUTPUT_GAIN_STEP, [](float v) { cfg_sm.p_band_dac_offset.set(v); }};
static float_slider_t codec_gain_fs = {OUTPUT_GAIN_STEP, [](float v) { cfg_sm.p_output_gain.set(v); }};
static float_slider_t cessb_fs      = {0.1f, [](float v) { cfg_sm.p_cessb_power_up.set(v); }};

static void iq_offset_cb(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);
    int32_t   v = lv_slider_get_value(s);

    slider_text(s, "%d", v);
    ((ParamInt *)lv_event_get_user_data(e))->set(v * TX_OFFSET_SCALE);
}

/* Another band, other values: the sliders follow (their labels with the event) */
static void iq_offset_follow(Subject *subj, void *slider) {
    lv_slider_set_value((lv_obj_t *)slider, static_cast<SubjectInt *>(subj)->get() / TX_OFFSET_SCALE, LV_ANIM_OFF);
    lv_event_send((lv_obj_t *)slider, LV_EVENT_VALUE_CHANGED, NULL);
}

static void dac_gain_follow(Subject *subj, void *slider) {
    lv_slider_set_value((lv_obj_t *)slider, cfg_sm.p_band_dac_offset.get() / OUTPUT_GAIN_STEP, LV_ANIM_OFF);
    lv_event_send((lv_obj_t *)slider, LV_EVENT_VALUE_CHANGED, NULL);
}

static uint8_t make_iq_offsets(uint8_t row) {
    ParamInt *p[2] = {&cfg_sm.p_band_tx_i_offset, &cfg_sm.p_band_tx_q_offset};

    wl_row_label(row, "IQ offsets");
    for (int i = 0; i < 2; i++) {
        int32_t   v = p[i]->get() / TX_OFFSET_SCALE;
        lv_obj_t *s = row_slider(row_cell(row, 1 + 3 * i, 3, SMALL_3), v, -10000 / TX_OFFSET_SCALE,
                                 10000 / TX_OFFSET_SCALE, SMALL_3 - 110, iq_offset_cb, p[i], "%d", v);

        observers.emplace_back(p[i]->subscribe(iq_offset_follow, s));
    }
    return row + 1;
}

static uint8_t make_band_gain(uint8_t row) {
    lv_obj_t *s;

    wl_row_label(row, "Band output gain");
    s = float_slider(row_cell(row, 1, 3, SMALL_6), cfg_sm.p_band_dac_offset.get(), -6.0f, 6.0f, SMALL_6 - 120,
                     &dac_gain_fs);
    observers.emplace_back(cfg_sm.p_band_dac_offset.subscribe(dac_gain_follow, s));
    return row + 1;
}

static uint8_t make_codec_gain(uint8_t row) {
    wl_row_label(row, "Codec gain");
    float_slider(row_cell(row, 1, 3, SMALL_6), cfg_sm.p_output_gain.get(), -25.0f, 25.0f, SMALL_6 - 120, &codec_gain_fs);
    return row + 1;
}

static uint8_t make_cessb(uint8_t row) {
    wl_row_label(row, "CESSB on, level");
    param_switch(row, 1, 2, SMALL_2, cfg_sm.p_cessb_on);
    float_slider(row_cell(row, 3, 4, SMALL_4), cfg_sm.p_cessb_power_up.get(), 0.0f, 5.0f, SMALL_3 - 40, &cessb_fs);
    return row + 1;
}

/* Bluetooth CAT (btcat) */

#define BTCAT_CTL "/usr/sbin/btcat-ctl"

static lv_obj_t *bt_visible_sw = NULL;
static lv_obj_t *bt_visible_label;
static lv_timer_t *bt_visible_timer;
static uint32_t bt_visible_tick;     /* last tap: btcat-ctl gets 3 s to write the state */

static bool btcat_read(const char *key, bool def) {
    const char *files[] = { "/etc/default/btcat", "/mnt/btcat.conf" };
    bool val = def;
    size_t klen = strlen(key);

    for (auto f : files) {
        FILE *fp = fopen(f, "r");
        if (!fp) continue;
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
                const char *v = line + klen + 1;
                if (*v == '"') v++;
                val = strncmp(v, "yes", 3) == 0;
            }
        }
        fclose(fp);
    }
    return val;
}

static void btcat_run(const char *what, bool on) {
    std::string cmd = std::string(BTCAT_CTL " ") + what + (on ? " on" : " off") + " >/dev/null 2>&1";
    std::thread([cmd]() { system(cmd.c_str()); }).detach();
}

static void bt_power_update_cb(lv_event_t * e) {
    lv_obj_t *obj = lv_event_get_target(e);
    bool on = lv_obj_has_state(obj, LV_STATE_CHECKED);

    btcat_run("bt", on);
    if (bt_visible_sw) {
        if (on) lv_obj_clear_state(bt_visible_sw, LV_STATE_DISABLED);
        else    lv_obj_add_state(bt_visible_sw, LV_STATE_DISABLED);
    }
}

static void bt_visible_update_cb(lv_event_t * e) {
    lv_obj_t *obj = lv_event_get_target(e);
    btcat_run("discoverable", lv_obj_has_state(obj, LV_STATE_CHECKED));
    bt_visible_tick = lv_tick_get();
}

/* The pairing window (3 min) as it really is: btcat-ctl keeps its end, in s
 * of uptime, in /tmp/btcat-pairing while it is open */
static void bt_visible_timer_cb(lv_timer_t *t) {
    int left = bt_dev_visible_s();

    if (lv_tick_elaps(bt_visible_tick) < 3000) return;
    if (left > 0) {
        lv_label_set_text_fmt(bt_visible_label, "Pairing %i:%02i", left / 60, left % 60);
        lv_obj_add_state(bt_visible_sw, LV_STATE_CHECKED);
    } else {
        lv_label_set_text(bt_visible_label, "Pairing (3 min)");
        lv_obj_clear_state(bt_visible_sw, LV_STATE_CHECKED);
    }
}

static void bt_visible_delete_cb(lv_event_t * e) {
    bt_visible_sw = NULL;
    lv_timer_del(bt_visible_timer);
}

static lv_obj_t * bt_switch_cell(uint8_t row, uint8_t col, bool state, lv_event_cb_t cb) {
    return row_switch(row, col, 3, SMALL_3, state, cb, NULL);
}

static uint8_t make_bt_power(uint8_t row) {
    lv_obj_t *obj = lv_label_create(grid);

    lv_label_set_text(obj, "Bluetooth");
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1);
    bt_switch_cell(row, 1, btcat_read("BTCAT_BT", true), bt_power_update_cb);
    return row + 1;
}

static uint8_t make_bt_pairing(uint8_t row) {
    lv_obj_t *obj;
    bool      power = btcat_read("BTCAT_BT", true);

    obj = lv_label_create(grid);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1);
    bt_visible_label = obj;
    bt_visible_sw = bt_switch_cell(row, 1, false, bt_visible_update_cb);
    lv_obj_add_event_cb(bt_visible_sw, bt_visible_delete_cb, LV_EVENT_DELETE, NULL);
    bt_visible_tick = lv_tick_get() - 3000;
    bt_visible_timer = lv_timer_create(bt_visible_timer_cb, 1000, NULL);
    bt_visible_timer_cb(bt_visible_timer);
    if (!power) lv_obj_add_state(bt_visible_sw, LV_STATE_DISABLED);

    return row + 1;
}

/* The charger: the radio is told it is on with On only */
static void charger_cb(lv_event_t *e) {
    uint8_choice_cb(e);
    radio_set_charger(params.charger.x == RADIO_CHARGER_ON);
}

static uint8_t make_charger(uint8_t row) {
    wl_row_label(row, "Charger");
    row_choice(row, 1, 6, SMALL_6, " Off \n On \n Shadow", params.charger.x, charger_cb, &params.charger);
    return row + 1;
}

static uint8_t make_freq_accel(uint8_t row) {
    wl_row_label(row, "Freq acceleration");
    row_choice(row, 1, 6, SMALL_6, " None \n Lite \n Strong", params.freq_accel.x, uint8_choice_cb, &params.freq_accel);
    return row + 1;
}

/* Screen recordings: a new file after this long */
static uint8_t make_screen_rec_split(uint8_t row) {
    wl_row_label(row, "File length");
    row_choice(row, 1, 6, SMALL_6, " 1 hour \n 45 min \n 30 min \n 15 min \n 5 min ", params.screen_rec_split.x,
               uint8_choice_cb, &params.screen_rec_split);
    return row + 1;
}

/* The built-in themes and the files in /mnt/themes */
static uint8_t make_theme(uint8_t row) {
    char        names[16][32];
    int         n = theme_list(names, 16);
    std::string options;
    uint16_t    sel = 0;

    for (int i = 0; i < n; i++) {
        options += std::string(i ? "\n" : "") + names[i];
        if (strcasecmp(names[i], params.theme_name.x) == 0) {
            sel = i;
        }
    }
    wl_row_label(row, "Theme");
    theme_dd = row_choice(row, 1, 6, SMALL_6, options.c_str(), sel, theme_update_cb, NULL);
    return row + 1;
}

static uint8_t make_font(uint8_t row) {
    wl_row_label(row, "Font");
    font_dd = row_choice(row, 1, 6, SMALL_6, " Sony Sketch \n Roboto \n DejaVu", params.font.x, font_update_cb, &params.font);
    return row + 1;
}

/* Wireless (WiFi, Ethernet) and Bluetooth. The MFK walks the rows; a press
 * on a network or device opens its action menu. */

#define WL_ROW_H    40
#define WIFI_ROWS   8
#define BT_ROWS     8

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *name;
    lv_obj_t *pill_1;
    lv_obj_t *pill_2;
    lv_obj_t *note;
    lv_obj_t *bars[4];
    lv_obj_t *lock;
} wl_row_t;

typedef struct {
    std::string           label;
    std::string           note;
    std::function<void()> fn;
} wl_item_t;

static int                    wl_gen;       /* results for a deleted page are dropped */
static lv_timer_t            *wl_timer;
static bool                   wl_kbd;       /* password keyboard open */
static wifi_ap_info_t         wl_kbd_ap;

static lv_obj_t              *wl_menu;      /* action menu */
static lv_obj_t              *wl_menu_back; /* row focused again when it closes */
static std::vector<wl_item_t> wl_items;
static std::vector<lv_obj_t *> wl_item_rows;
static int                    wl_sel;
static const char            *wl_tab = "Wireless";  /* its button closes the menu */

static struct {
    lv_obj_t      *status_pill;
    lv_obj_t      *status_ssid;
    lv_obj_t      *ip;
    lv_obj_t      *scan_btn;
    lv_obj_t      *scan_note;
    wl_row_t       rows[WIFI_ROWS];
    wifi_ap_info_t aps[WIFI_ROWS];
    bool           scanning;
    time_t         last_scan;
} wf;

static lv_timer_t *bt_timer;     /* Bluetooth page */
static int         bt_ticks;

static struct {
    lv_obj_t             *pill;     /* adapter */
    lv_obj_t             *state;
    lv_obj_t             *addr;
    lv_obj_t             *cat;
    lv_obj_t             *scan_btn;
    lv_obj_t             *scan_note;
    lv_obj_t             *rx;
    lv_obj_t             *vol;
    lv_obj_t             *mute;
    wl_row_t              rows[BT_ROWS];
    std::vector<bt_dev_t> devs;
    std::vector<bt_dev_t> found;    /* not paired, seen since the last Scan */
    std::string           note;     /* after a scan that found nothing */
    bool                  was_scanning;
    int                   vol_pending = -1;
    bool                  vol_busy;
} bt;

static lv_obj_t * wl_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, lv_coord_t x) {
    lv_obj_t *obj = lv_label_create(parent);

    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_align(obj, LV_ALIGN_LEFT_MID, x, 0);
    lv_label_set_text(obj, "");
    return obj;
}

static lv_obj_t * wl_pill_create(lv_obj_t *parent, lv_coord_t x) {
    lv_obj_t *obj = wl_label(parent, SF(18), theme_hex(TC_text), x);

    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_pad_hor(obj, 8, 0);
    lv_obj_set_style_pad_ver(obj, 2, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

static void wl_pill(lv_obj_t *obj, const char *text, uint32_t bg, uint32_t fg) {
    if (!text) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(obj, text);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(fg), 0);
}

#define PILL_OK     "connected", theme_hex(TC_ok_bg), theme_hex(TC_ok)
#define PILL_HP     "headphones", theme_hex(TC_pill_hp), theme_hex(TC_accent)
#define PILL_CAT    "CAT", theme_hex(TC_pill_cat), theme_hex(TC_pill_cat_text)
#define PILL_PHONE  "phone", theme_hex(TC_pill_cat), theme_hex(TC_pill_cat_text)
#define PILL_KBD    "keyboard", theme_hex(TC_pill_kbd), theme_hex(TC_pill_kbd_text)
#define PILL_NEW    "new", theme_hex(TC_chip), theme_hex(TC_text_value)

/* Sections: a spaced title with a line to the right edge, then the rows on
 * their own grid. `grid` is the section being filled, the rows go from row 1. */

#define CARD_HEAD_H     30
#define ROW_LABEL       LV_OBJ_FLAG_USER_1     /* a row's label, with a dotted leader to its control */

static lv_coord_t *card_cols;

static uint8_t card_begin(const char *title, lv_coord_t *cols = col_dsc) {
    lv_obj_t *obj;

    grid = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(grid, TC(text), 0);
    lv_obj_set_style_pad_hor(grid, 4, 0);
    lv_obj_set_style_pad_column(grid, SMALL_PAD, 0);
    lv_obj_set_style_pad_row(grid, 5, 0);

    card_cols = cols;
    row_dsc = row_pool[card_n++];
    std::fill_n(row_dsc, 64, ROW_H - 1);
    row_dsc[0] = CARD_HEAD_H;

    int ncols = 0;
    while (cols[ncols] != LV_GRID_TEMPLATE_LAST) ncols++;

    obj = lv_obj_create(grid);
    lv_obj_remove_style_all(obj);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 10, 0);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_STRETCH, 0, ncols, LV_GRID_ALIGN_STRETCH, 0, 1);

    lv_obj_t *lbl = lv_label_create(obj);
    lv_obj_set_style_text_font(lbl, SF(18), 0);
    lv_obj_set_style_text_color(lbl, TC(text_head), 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_label_set_text(lbl, title);

    lv_obj_t *line = lv_obj_create(obj);
    lv_obj_remove_style_all(line);
    lv_obj_set_height(line, 1);
    lv_obj_set_flex_grow(line, 1);
    lv_obj_set_style_bg_color(line, TC(card), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    return 1;
}

/* Dots every 7 px from each row label to the first thing on its right */
static void leader_draw_cb(lv_event_t *e) {
    lv_obj_t          *g   = lv_event_get_target(e);
    uint32_t           n   = lv_obj_get_child_cnt(g);
    lv_draw_rect_dsc_t dot;

    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = TC(text_faint);

    for (uint32_t i = 1; i < n; i++) {
        lv_obj_t *lbl = lv_obj_get_child(g, i);

        if (!lv_obj_has_flag(lbl, ROW_LABEL)) continue;

        int        r  = lv_obj_get_style_grid_cell_row_pos(lbl, 0);
        lv_coord_t x2 = LV_COORD_MAX;

        for (uint32_t j = 1; j < n; j++) {
            lv_obj_t *c = lv_obj_get_child(g, j);

            if (lv_obj_get_style_grid_cell_row_pos(c, 0) == r && lv_obj_get_style_grid_cell_column_pos(c, 0) >= 1 &&
                !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
                x2 = LV_MIN(x2, c->coords.x1);
            }
        }
        if (x2 == LV_COORD_MAX) continue;

        lv_coord_t y = lbl->coords.y2 - 7;

        for (lv_coord_t x = lbl->coords.x2 + 10; x + 10 < x2; x += 7) {
            lv_area_t a;

            lv_area_set(&a, x, y, x + 1, y + 1);
            lv_draw_rect(lv_event_get_draw_ctx(e), &dot, &a);
        }
    }
}

/* Four corner brackets around the focused control, the choices with a triangle each side */
static void bracket_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);

    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(e, 6);
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_DRAW_POST_END || !lv_obj_has_state(obj, LV_STATE_FOCUSED)) {
        return;
    }
    lv_draw_ctx_t     *ctx = lv_event_get_draw_ctx(e);
    lv_area_t          o   = obj->coords;
    bool               choice = lv_obj_check_type(obj, &lv_dropdown_class);
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = TC(accent);
    lv_area_increase(&o, 4, 2);

    if (choice) {   /* tight around the value, right-aligned */
        char       buf[64];
        lv_point_t size;

        lv_dropdown_get_selected_str(obj, buf, sizeof(buf));
        lv_txt_get_size(&size, buf, lv_obj_get_style_text_font(obj, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        o.x1 = LV_MAX(o.x1, o.x2 - size.x - 2 * 26);
    } else if (lv_obj_check_type(obj, &lv_btn_class) && !lv_obj_has_flag(obj, LV_OBJ_FLAG_CHECKABLE) &&
               lv_obj_get_child_cnt(obj)) {
        o.x1 = LV_MAX(o.x1, lv_obj_get_child(obj, 0)->coords.x1 - 12);
    }

    const int L = 9;
    const int bars[8][4] = {
        {o.x1, o.y1, o.x1 + L, o.y1 + 1}, {o.x1, o.y1, o.x1 + 1, o.y1 + L},
        {o.x2 - L, o.y1, o.x2, o.y1 + 1}, {o.x2 - 1, o.y1, o.x2, o.y1 + L},
        {o.x1, o.y2 - 1, o.x1 + L, o.y2}, {o.x1, o.y2 - L, o.x1 + 1, o.y2},
        {o.x2 - L, o.y2 - 1, o.x2, o.y2}, {o.x2 - 1, o.y2 - L, o.x2, o.y2},
    };
    for (auto &b : bars) {
        lv_area_t a;

        lv_area_set(&a, b[0], b[1], b[2], b[3]);
        lv_draw_rect(ctx, &dsc, &a);
    }
    if (choice) {
        int y = (o.y1 + o.y2) / 2;

        for (int side : {-1, 1}) {
            int        x = side < 0 ? o.x1 + 7 : o.x2 - 7;
            lv_point_t t[3];

            t[0].x = x;                 t[0].y = y;
            t[1].x = x - side * 7;      t[1].y = y - 6;
            t[2].x = x - side * 7;      t[2].y = y + 6;
            lv_draw_polygon(ctx, &dsc, t, 3);
        }
    }
}

/* Controls flat on the row: no frames, values as bright text, brackets for the focus.
 * Sliders and switches keep their look, toggles a thin frame (off would vanish). */
static void flat_ctl(lv_obj_t *obj) {
    bool text = lv_obj_check_type(obj, &lv_dropdown_class) || lv_obj_check_type(obj, &lv_spinbox_class) ||
                lv_obj_check_type(obj, &lv_btn_class);

    if (!text && !lv_obj_check_type(obj, &lv_slider_class) && !lv_obj_check_type(obj, &lv_switch_class)) {
        if (lv_obj_check_type(obj, &lv_obj_class) && !lv_obj_get_group(obj)) {    /* a cell: look inside */
            lv_obj_set_style_border_width(obj, 0, 0);
            for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) flat_ctl(lv_obj_get_child(obj, i));
        }
        return;
    }
    lv_obj_add_event_cb(obj, bracket_cb, LV_EVENT_ALL, NULL);
    lv_obj_refresh_ext_draw_size(obj);
    if (!text) {    /* their cell no longer frames them: a track of their own */
        lv_obj_set_style_bg_color(obj, TC(track), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        return;
    }

    bool toggle = lv_obj_has_flag(obj, LV_OBJ_FLAG_CHECKABLE);

    lv_obj_set_style_border_width(obj, toggle ? 1 : 0, 0);
    lv_obj_set_style_border_color(obj, TC(card), 0);
    lv_obj_set_style_border_width(obj, toggle ? 1 : 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(obj, TC(accent), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(obj, TC(accent), LV_STATE_FOCUSED);

    if (lv_obj_check_type(obj, &lv_dropdown_class)) {   /* the value at the right, room for the triangle */
        lv_dropdown_set_symbol(obj, LV_SYMBOL_DUMMY);
        lv_obj_set_style_base_dir(obj, LV_BASE_DIR_RTL, 0);
        lv_obj_set_style_pad_right(obj, 26, 0);
    } else if (lv_obj_check_type(obj, &lv_spinbox_class)) {
        lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(obj, TC(accent), LV_PART_CURSOR | LV_STATE_FOCUSED);   /* no fill behind it now */
        lv_obj_set_style_text_color(obj, TC(focus_text), LV_PART_CURSOR | LV_STATE_EDITED);
    } else if (!toggle && lv_obj_get_child_cnt(obj)) {
        lv_obj_align(lv_obj_get_child(obj, 0), LV_ALIGN_RIGHT_MID, -6, 0);
    }
}

/* Rows go up to `row` (exclusive): put a lone control at the right edge, a dotted
 * leader from each row label to its control, the controls flat on the row. */
static void card_end(uint8_t row) {
    uint8_t  ctl[64] = {};
    int      ncols = 0;
    uint32_t n = lv_obj_get_child_cnt(grid);

    while (card_cols[ncols] != LV_GRID_TEMPLATE_LAST) ncols++;
    row_dsc[row] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(grid, card_cols, row_dsc);

    for (uint32_t i = 1; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(grid, i);

        if (lv_obj_get_style_grid_cell_column_pos(c, 0) >= 1) {
            ctl[lv_obj_get_style_grid_cell_row_pos(c, 0)]++;
        }
    }
    for (uint32_t i = 1; i < n; i++) {
        lv_obj_t *c    = lv_obj_get_child(grid, i);
        int       pos  = lv_obj_get_style_grid_cell_column_pos(c, 0);
        int       span = lv_obj_get_style_grid_cell_column_span(c, 0);
        int       r    = lv_obj_get_style_grid_cell_row_pos(c, 0);
        lv_style_value_t v;

        if (pos == 0 && span == 1 && lv_obj_check_type(c, &lv_label_class) &&
            lv_obj_get_local_style_prop(c, LV_STYLE_TEXT_COLOR, &v, 0) != LV_RES_OK) {
            lv_obj_add_flag(c, ROW_LABEL);
            lv_obj_set_style_text_color(c, TC(text_value), 0);
        } else if (pos >= 1 && ctl[r] == 1 && pos + span < ncols) {
            lv_coord_t w = lv_obj_get_style_width(c, 0);

            if (LV_COORD_IS_PX(w) && w > span * (SMALL_1 + SMALL_PAD) - SMALL_PAD) {
                span = ncols - 1;   /* wider than its cells */
            }
            lv_obj_set_grid_cell(c, (lv_grid_align_t)lv_obj_get_style_grid_cell_x_align(c, 0), ncols - span, span,
                                 (lv_grid_align_t)lv_obj_get_style_grid_cell_y_align(c, 0), r, 1);
        }
        flat_ctl(c);
    }
    lv_obj_add_event_cb(grid, leader_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
}

static void wl_row_label(uint8_t row, const char *text) {
    lv_obj_t *obj = lv_label_create(grid);

    lv_label_set_text(obj, text);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1);
}

static lv_obj_t * wl_button(uint8_t row, const char *text, lv_event_cb_t cb) {
    lv_obj_t *obj = lv_btn_create(grid);

    lv_obj_remove_style_all(obj);
    dialog_item(&dialog, obj);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, TC(control_border), 0);
    lv_obj_set_size(obj, SMALL_3, ROW_H - 4);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 3, LV_GRID_ALIGN_CENTER, row, 1);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(obj);
    lv_obj_set_style_text_font(label, SF(22), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return obj;
}

/* Note to the right of a 3-column control */
static lv_obj_t * wl_note(uint8_t row) {
    lv_obj_t *obj = wl_label(grid, theme_shape.vision ? &sony_18 : &sony_20, theme_hex(TC_text_dim), 0);   /* one size up: two would overflow */

    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 4, 3, LV_GRID_ALIGN_CENTER, row, 1);
    return obj;
}

/* A list (all the columns) of hidden rows; the refresh fills and shows them */
static lv_obj_t * wl_list(uint8_t row) {
    lv_obj_t *obj = lv_obj_create(grid);

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(obj, 2, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_STRETCH, 0, 7, LV_GRID_ALIGN_START, row, 1);
    row_dsc[row] = LV_GRID_CONTENT;
    return obj;
}

static void wl_row_create(wl_row_t *r, lv_obj_t *list, lv_event_cb_t cb, int i) {
    lv_obj_t *obj = lv_obj_create(list);

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, lv_pct(100), WL_ROW_H);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);   /* a MFK press is a click */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(obj, 4, 0);
    lv_obj_set_style_bg_color(obj, TC(sel_bg), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(obj, TC(sel_border), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, obj);

    r->obj = obj;
    r->name = wl_label(obj, SF(22), theme_hex(TC_text), 0);      /* lined up with the row labels */
    lv_obj_set_size(r->name, 270, lv_font_get_line_height(SF(22)));   /* one line, then dots */
    lv_label_set_long_mode(r->name, LV_LABEL_LONG_DOT);
}

/* Hidden rows can't have the focus: move it before hiding the focused one */
static void wl_row_show(wl_row_t *r, bool show, lv_obj_t *fallback) {
    if (!show && lv_group_get_focused(keyboard_group) == r->obj) {
        lv_group_focus_obj(fallback);
    }
    if (show) {
        lv_obj_clear_flag(r->obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r->obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Action menu */

static void wl_menu_show_sel() {
    for (size_t i = 0; i < wl_item_rows.size(); i++) {
        bool on = (int)i == wl_sel;

        lv_obj_set_style_bg_opa(wl_item_rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wl_item_rows[i], on ? 2 : 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(wl_item_rows[i], 0), on ? TC(sel_text) : TC(text), 0);
    }
}

static void wl_menu_close() {
    if (!wl_menu) {
        return;
    }
    lv_obj_del(wl_menu);
    wl_menu = NULL;
    wl_item_rows.clear();
    if (wl_menu_back) {
        lv_group_focus_obj(wl_menu_back);
    }
    lv_group_set_editing(keyboard_group, false);
}

/* Run after the event: closing deletes the menu that got it */
static void wl_menu_pick_async(void *arg) {
    if (!wl_menu) {
        return;
    }
    int sel = (int)(intptr_t)arg;
    auto fn = sel >= 0 ? wl_items[sel].fn : nullptr;

    wl_menu_close();
    if (fn) {
        fn();
    }
}

static void wl_menu_key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            if (wl_sel > 0) wl_sel--;
            wl_menu_show_sel();
            break;

        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            if (wl_sel < (int)wl_items.size() - 1) wl_sel++;
            wl_menu_show_sel();
            break;

        case LV_KEY_ENTER:
            lv_async_call(wl_menu_pick_async, (void *)(intptr_t)wl_sel);
            break;

        case LV_KEY_ESC:
            lv_async_call(wl_menu_pick_async, (void *)(intptr_t)-1);
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

static void wl_menu_click_cb(lv_event_t *e) {
    lv_obj_t   *target = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_get_act();

    /* On the menu itself only the MFK press counts, not a touch */
    if (target == wl_menu && !(indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER)) {
        return;
    }

    for (size_t i = 0; i < wl_item_rows.size(); i++) {
        if (wl_item_rows[i] == target) {
            wl_sel = i;
        }
    }
    lv_async_call(wl_menu_pick_async, (void *)(intptr_t)wl_sel);
}

static void wl_menu_open(const std::string &title, std::vector<wl_item_t> items) {
    const lv_coord_t w = theme_shape.vision ? 480 : 420, head_h = 36, item_h = 40, hint_h = 30;

    wl_menu_close();
    wl_items = std::move(items);
    wl_menu_back = lv_group_get_focused(keyboard_group);

    wl_menu = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(wl_menu);
    lv_obj_set_size(wl_menu, w, head_h + wl_items.size() * item_h + hint_h + 8);
    lv_obj_center(wl_menu);
    lv_obj_add_style(wl_menu, &popup_style, 0);
    lv_obj_clear_flag(wl_menu, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *obj = wl_label(wl_menu, SF(18), theme_hex(TC_text_head), 16);
    lv_obj_set_width(obj, w - 32);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_label_set_text(obj, title.c_str());
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, 16, 8);

    for (size_t i = 0; i < wl_items.size(); i++) {
        lv_obj_t *row = lv_obj_create(wl_menu);

        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w - 4, item_h);
        lv_obj_set_pos(row, 0, head_h + i * item_h);
        lv_obj_set_style_bg_color(row, TC(sel_bg), 0);
        lv_obj_set_style_border_color(row, TC(sel_border), 0);
        lv_obj_set_style_border_side(row, (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wl_menu_click_cb, LV_EVENT_CLICKED, NULL);

        obj = wl_label(row, SF(22), theme_hex(TC_text), 16);
        lv_label_set_text(obj, wl_items[i].label.c_str());

        obj = wl_label(row, SF(18), theme_hex(TC_text_dim), 0);
        lv_label_set_text(obj, wl_items[i].note.c_str());
        lv_obj_align(obj, LV_ALIGN_RIGHT_MID, -16, 0);

        wl_item_rows.push_back(row);
    }

    obj = wl_label(wl_menu, SF(16), theme_hex(TC_text_dim), 16);
    lv_label_set_text_fmt(obj, "MFK: choose - press: confirm - %s: close", wl_tab);
    lv_obj_align(obj, LV_ALIGN_BOTTOM_LEFT, 16, -8);

    wl_sel = 0;
    wl_menu_show_sel();

    lv_group_add_obj(keyboard_group, wl_menu);
    lv_obj_add_event_cb(wl_menu, wl_menu_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(wl_menu, wl_menu_click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(wl_menu);
    lv_group_set_editing(keyboard_group, true);
}

/* WiFi */

static int wifi_compare_aps(const void *a, const void *b) {
    const wifi_ap_info_t *ap_a = (const wifi_ap_info_t *)a;
    const wifi_ap_info_t *ap_b = (const wifi_ap_info_t *)b;
    int score_a = ap_a->is_connected * 2 + ap_a->known;
    int score_b = ap_b->is_connected * 2 + ap_b->known;

    if (score_a != score_b) {
        return score_b - score_a;
    }
    return ap_b->strength - ap_a->strength;
}

static void wifi_update() {
    static const char *status_names[] = { "off", "starting", "disconnected", "connecting", "connected" };
    wifi_status_t status = wifi_get_status();
    bool          on = status != WIFI_OFF && status != WIFI_STARTING;
    const char   *ssid = "";
    int           n = 0;

    if (status == WIFI_CONNECTED) {
        wl_pill(wf.status_pill, PILL_OK);
    } else {
        wl_pill(wf.status_pill, status_names[status], theme_hex(TC_chip), theme_hex(TC_text_value));
    }

    if (on) {
        wifi_ap_arr_t aps = wifi_get_available_access_points();

        if (aps.count) {
            qsort(aps.ap_arr, aps.count, sizeof(wifi_ap_info_t), wifi_compare_aps);
        }
        for (; n < aps.count && n < WIFI_ROWS; n++) {
            wf.aps[n] = aps.ap_arr[n];
        }
        wifi_aps_info_delete(aps);
    }

    for (int i = 0; i < WIFI_ROWS; i++) {
        wl_row_t       *r = &wf.rows[i];
        wifi_ap_info_t *ap = &wf.aps[i];

        wl_row_show(r, i < n, wf.scan_btn);
        if (i >= n) {
            continue;
        }
        lv_label_set_text(r->name, ap->ssid);
        for (int b = 0; b < 4; b++) {
            bool lit = b < (ap->strength + 24) / 25;

            lv_obj_set_style_bg_color(r->bars[b], lit ? TC(ok) : TC(off), 0);
        }
        if (ap->password_validator) {
            lv_obj_clear_flag(r->lock, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(r->lock, LV_OBJ_FLAG_HIDDEN);
        }
        if (ap->is_connected) {
            wl_pill(r->pill_1, PILL_OK);
            ssid = ap->ssid;
        } else {
            wl_pill(r->pill_1, NULL, 0, 0);
        }
        lv_label_set_text(r->note, ap->known ? "saved" : "");
    }
    lv_label_set_text(wf.status_ssid, ssid);

    char  ip[16] = "", gw[16] = "";
    char *ip_p = ip, *gw_p = gw;

    if (status == WIFI_CONNECTED && wifi_get_ipaddr(&ip_p, &gw_p)) {
        lv_label_set_text_fmt(wf.ip, "%s  /  %s", ip, gw);
    } else {
        lv_label_set_text(wf.ip, "-");
    }

    bool scanning = on && wifi_scanning();

    if (wf.scanning && !scanning) {
        wf.last_scan = time(NULL);
    }
    wf.scanning = scanning;
    if (scanning) {
        lv_label_set_text(wf.scan_note, "scanning...");
    } else if (on && wf.last_scan) {
        lv_label_set_text_fmt(wf.scan_note, "last scan %i s ago", (int)(time(NULL) - wf.last_scan));
    } else {
        lv_label_set_text(wf.scan_note, "");
    }
}

static void wifi_power_cb(lv_event_t *e) {
    if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)) {
        wifi_power_on();
        msg_update_text_fmt("Turning on");
    } else {
        wifi_power_off();
    }
    wifi_update();
}

static void wifi_scan_cb(lv_event_t *e) {
    wifi_status_t status = wifi_get_status();

    if (status == WIFI_OFF || status == WIFI_STARTING || wifi_scanning()) {
        return;
    }
    wifi_start_scan();
}

static bool wl_kbd_close() {
    wl_kbd = false;
    textarea_window_close();
    make_wireless_page();
    return true;
}

static bool wl_kbd_ok() {
    const char *password = textarea_window_get();

    if (wl_kbd_ap.password_validator && !wl_kbd_ap.password_validator(password)) {
        msg_update_text_fmt("Incorrect password");
        return false;
    }
    if (wl_kbd_ap.known) {
        wifi_update_connection(wl_kbd_ap.ssid, password);
    } else {
        wifi_add_connection(wl_kbd_ap.ssid, password);
    }
    return wl_kbd_close();
}

/* The keyboard gets all the keys: the page is built again when it closes */
static void wl_kbd_open(const wifi_ap_info_t &ap) {
    wl_kbd_ap = ap;
    wl_kbd = true;
    lv_group_remove_all_objs(keyboard_group);
    textarea_window_open(wl_kbd_ok, wl_kbd_close);
    lv_textarea_set_placeholder_text(textarea_window_text(), " Password");
}

static void wifi_row_cb(lv_event_t *e) {
    wifi_ap_info_t         ap = wf.aps[(intptr_t)lv_event_get_user_data(e)];
    std::vector<wl_item_t> items;

    if (ap.is_connected) {
        items.push_back({"Disconnect", "", [] { wifi_disconnect(); }});
    } else {
        items.push_back({"Connect", "", [ap] {
            if (ap.known) {
                wifi_connect(ap.ssid);
            } else if (!ap.password_validator) {
                wifi_add_connection(ap.ssid, NULL);
            } else {
                wl_kbd_open(ap);
            }
        }});
    }
    if (ap.known && ap.password_validator) {
        items.push_back({"Change password", "", [ap] { wl_kbd_open(ap); }});
    }
    if (ap.known) {
        items.push_back({"Delete", "", [ap] {
            wifi_delete_connection(ap.ssid);
            msg_update_text_fmt("Connection deleted");
        }});
    }
    wl_menu_open(ap.ssid, items);
}

static uint8_t make_wifi(uint8_t row) {
    lv_obj_t *obj;

    wl_row_label(row, "WiFi/BT power");
    bt_switch_cell(row, 1, params.wifi_enabled.x, wifi_power_cb);
    row++;

    wl_row_label(row, "Status");
    obj = lv_obj_create(grid);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 12, 0);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    wf.status_pill = wl_pill_create(obj, 0);
    wf.status_ssid = wl_label(obj, SF(22), theme_hex(TC_text), 0);
    row++;

    obj = wl_label(grid, SF(22), theme_hex(TC_text_dim), 0);
    lv_label_set_text(obj, "IP / Gateway");
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1);
    wf.ip = wl_label(grid, SF(22), theme_hex(TC_text_dim), 0);
    lv_obj_set_grid_cell(wf.ip, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    row++;

    wl_row_label(row, "Networks");
    wf.scan_btn = wl_button(row, "Scan", wifi_scan_cb);
    wf.scan_note = wl_note(row);
    row++;

    lv_obj_t *list = wl_list(row);

    for (int i = 0; i < WIFI_ROWS; i++) {
        wl_row_t *r = &wf.rows[i];

        wl_row_create(r, list, wifi_row_cb, i);
        for (int b = 0; b < 4; b++) {
            obj = lv_obj_create(r->obj);
            lv_obj_remove_style_all(obj);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
            lv_obj_set_size(obj, 8, 6 + b * 5);
            lv_obj_align(obj, LV_ALIGN_BOTTOM_LEFT, 300 + b * 11, -9);
            r->bars[b] = obj;
        }
        /* padlock: shackle and body */
        r->lock = lv_obj_create(r->obj);
        lv_obj_remove_style_all(r->lock);
        lv_obj_set_size(r->lock, 14, 20);
        lv_obj_align(r->lock, LV_ALIGN_LEFT_MID, 380, 0);
        obj = lv_obj_create(r->lock);
        lv_obj_remove_style_all(obj);
        lv_obj_set_size(obj, 10, 12);
        lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(obj, 5, 0);
        lv_obj_set_style_border_width(obj, 2, 0);
        lv_obj_set_style_border_color(obj, TC(lock), 0);
        obj = lv_obj_create(r->lock);
        lv_obj_remove_style_all(obj);
        lv_obj_set_size(obj, 14, 11);
        lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_radius(obj, 2, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(obj, TC(lock), 0);

        r->pill_1 = wl_pill_create(r->obj, 440);
        r->note = wl_label(r->obj, SF(20), theme_hex(TC_text_dim), 620);
    }
    return row + 1;
}

/* Bluetooth devices (bt_dev.c) and headphones (btaudio-ctl) */

static std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    size_t                   start = 0, pos;

    while ((pos = s.find(sep, start)) != std::string::npos) {
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    out.push_back(s.substr(start));
    return out;
}

static void bt_update_view() {
    if (!bt.scan_btn) {
        return;     /* page gone */
    }
    size_t n = bt.devs.size();

    for (size_t i = 0; i < BT_ROWS; i++) {
        wl_row_t *r = &bt.rows[i];

        wl_row_show(r, i < n, bt.scan_btn);
        if (i >= n) {
            continue;
        }
        bt_dev_t *d = &bt.devs[i];

        lv_label_set_text(r->name, d->name);
        if (!d->paired) {
            wl_pill(r->pill_1, PILL_NEW);
        } else if (d->audio) {
            wl_pill(r->pill_1, PILL_HP);
        } else if (d->input) {
            wl_pill(r->pill_1, PILL_KBD);
        } else if (d->phone) {
            wl_pill(r->pill_1, PILL_PHONE);
        } else {
            wl_pill(r->pill_1, PILL_CAT);
        }
        wl_pill(r->pill_2, d->connected ? "connected" : NULL, theme_hex(TC_ok_bg), theme_hex(TC_ok));
        if (!d->paired) {
            lv_label_set_text(r->note, "press to pair");
        } else if (!d->connected) {
            lv_label_set_text(r->note, "not connected");
        } else {
            lv_label_set_text(r->note, "");
        }
        lv_obj_align(r->note, LV_ALIGN_LEFT_MID, d->connected ? 580 : 430, 0);
    }
}

/* Headphone volume and RX switch */
static void bt_status_done(const char *out, int status, void *user) {
    if ((intptr_t)user != wl_gen) {
        return;
    }
    for (auto &line : split(out, '\n')) {
        auto f = split(line, '\t');

        if (f[0] == "HP" && f.size() >= 3) {
            int vol = atoi(f[2].c_str());

            if (bt.vol && vol >= 0 && bt.vol_pending < 0 && !bt.vol_busy && !lv_obj_has_state(bt.vol, LV_STATE_EDITED)) {
                lv_slider_set_value(bt.vol, vol, LV_ANIM_OFF);
                lv_label_set_text_fmt((lv_obj_t *)lv_obj_get_user_data(bt.vol), "%i %%", vol);
            }
        } else if (bt.rx && f[0] == "RX" && f.size() >= 2) {
            if (f[1] == "yes") {
                lv_obj_add_state(bt.rx, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(bt.rx, LV_STATE_CHECKED);
            }
        }
    }
}

/* Bluetooth page, every second: the list and what the worker does (bt_dev.c).
 * The note shows its activity, a keyboard's code to type included. */
static void bt_update() {
    bt_dev_t    d;
    char        activity[48];
    bool        scanning = bt_dev_scanning();
    std::string focused;    /* the list re-sorts while a scan finds devices: the focus stays on its device */

    for (size_t i = 0; i < bt.devs.size() && i < BT_ROWS; i++) {
        if (bt.scan_btn && lv_group_get_focused(keyboard_group) == bt.rows[i].obj) {
            focused = bt.devs[i].addr;
        }
    }
    bt.devs.clear();
    for (int i = 0; bt_dev_get(i, &d); i++) {
        bt.devs.push_back(d);
    }
    /* bluez forgets a scanned device ~30 s after the scan: it stays here until the next Scan */
    for (auto &f : bt.found) {
        bool listed = false;

        for (auto &p : bt.devs) {
            listed |= strcmp(p.addr, f.addr) == 0;
        }
        if (!listed) {
            bt.devs.push_back(f);
        }
    }
    bt.found.clear();
    for (auto &p : bt.devs) {
        if (!p.paired) {
            bt.found.push_back(p);
        }
    }
    if (bt.was_scanning && !scanning && bt.found.empty()) {
        bt.note = "no headphones or keyboards found";
    }
    bt.was_scanning = scanning;

    bt_dev_activity(activity, sizeof(activity));
    if (bt.scan_note) {
        lv_label_set_text(bt.scan_note, activity[0] ? activity : bt.note.empty() ? "-" : bt.note.c_str());
    }
    bt_update_view();
    for (size_t i = 0; !focused.empty() && i < bt.devs.size() && i < BT_ROWS; i++) {
        if (focused == bt.devs[i].addr) {
            if (lv_group_get_focused(keyboard_group) != bt.rows[i].obj) lv_group_focus_obj(bt.rows[i].obj);
            break;
        }
    }
}

static void bt_scan_cb(lv_event_t *e) {
    if (bt_dev_busy() || bt_dev_scanning()) {
        return;
    }
    bt.found.clear();
    bt.note.clear();
    bt_dev_scan();
}

/* "Connect at startup" list, kept by btaudio-ctl */
static bool bt_autoconn(const char *addr) {
    char  line[512];
    bool  on = false;
    FILE *f = fopen("/mnt/btaudio.conf", "r");

    if (!f) {
        return false;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "BTAUDIO_AUTOCONNECT=", 20) == 0) {
            on = strstr(line, addr) != NULL;
        }
    }
    fclose(f);
    return on;
}

static void bt_row_cb(lv_event_t *e) {
    size_t i = (intptr_t)lv_event_get_user_data(e);

    if (i >= bt.devs.size() || bt_dev_busy()) {
        return;
    }
    bt_dev_t d = bt.devs[i];

    bt.note.clear();
    if (!d.paired) {
        bt_dev_connect(d.addr);     /* pairs first */
        return;
    }

    std::vector<wl_item_t> items;

    if (d.connected) {
        items.push_back({"Disconnect", "", [d] { bt_dev_disconnect(d.addr); }});
    } else if (d.audio || d.input) {
        items.push_back({"Connect", "", [d] { bt_dev_connect(d.addr); }});
    }
    if (d.audio) {
        bool on = bt_autoconn(d.addr);

        items.push_back({"Connect at startup", on ? "on" : "off", [d, on] {
            btaudio_run((std::string("autoconnect ") + d.addr + (on ? " off" : " on")).c_str(), NULL, NULL);
        }});
    }
    /* Forget asks once more, Cancel first */
    items.push_back({"Forget", "", [d] {
        wl_menu_open(std::string("Forget ") + d.name + "?", {
            {"Cancel", "", nullptr},
            {"Forget", "pair again to use it", [d] { bt_dev_forget(d.addr); }},
        });
    }});
    wl_menu_open(std::string(d.name) + " - " + d.addr, items);
}

static void bt_rx_cb(lv_event_t *e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);

    btaudio_run(on ? "rx on" : "rx off", NULL, NULL);
}

static void bt_vol_send();

static void bt_vol_done(const char *out, int status, void *user) {
    bt.vol_busy = false;
    bt_vol_send();
}

/* One volume command at a time, the last value wins */
static void bt_vol_send() {
    if (bt.vol_busy || bt.vol_pending < 0) {
        return;
    }
    std::string args = "volume " + std::to_string(bt.vol_pending);

    bt.vol_pending = -1;
    bt.vol_busy = true;
    btaudio_run(args.c_str(), bt_vol_done, NULL);
}

static void bt_vol_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    int       vol = lv_slider_get_value(obj);

    lv_label_set_text_fmt((lv_obj_t *)lv_obj_get_user_data(obj), "%i %%", vol);
    bt.vol_pending = vol;
    bt_vol_send();
}

static void bt_mute_cb(lv_event_t *e) {
    btaudio_mute_set(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static uint8_t make_bt_devices(uint8_t row) {
    lv_obj_t *obj;

    wl_row_label(row, "Headphones, keyboards");
    bt.scan_btn = wl_button(row, "Scan", bt_scan_cb);
    lv_label_set_text(wl_note(row), "16 s");
    row++;

    /* What the worker does, a keyboard's code to type included: the whole row */
    obj = wl_label(grid, SF(22), theme_hex(TC_text_dim), 0);   /* own colour: no leader dots */
    lv_label_set_text(obj, "Activity");
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1);
    bt.scan_note = wl_label(grid, theme_shape.vision ? &sony_18 : &sony_20, theme_hex(TC_text), 0);
    lv_obj_set_grid_cell(bt.scan_note, LV_GRID_ALIGN_END, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    lv_label_set_long_mode(bt.scan_note, LV_LABEL_LONG_DOT);
    lv_obj_set_width(bt.scan_note, SMALL_6);
    lv_obj_set_style_text_align(bt.scan_note, LV_TEXT_ALIGN_RIGHT, 0);
    row++;

    lv_obj_t *list = wl_list(row);

    for (int i = 0; i < BT_ROWS; i++) {
        wl_row_t *r = &bt.rows[i];

        wl_row_create(r, list, bt_row_cb, i);
        r->pill_1 = wl_pill_create(r->obj, 300);
        r->pill_2 = wl_pill_create(r->obj, 430);
        r->note = wl_label(r->obj, SF(20), theme_hex(TC_text_dim), 430);
    }
    return row + 1;
}

static uint8_t make_headphones(uint8_t row) {
    lv_obj_t *obj;

    wl_row_label(row, "RX to headphones");
    bt.rx = bt_switch_cell(row, 1, false, bt_rx_cb);
    row++;

    wl_row_label(row, "Volume");
    obj = lv_obj_create(grid);
    lv_obj_set_size(obj, SMALL_6, ROW_H);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    bt.vol = row_slider(obj, 50, 0, 100, SMALL_4, bt_vol_cb, NULL, "%i %%", 50);
    lv_obj_align(bt.vol, LV_ALIGN_LEFT_MID, 10, 0);
    row++;

    wl_row_label(row, "Mute speaker");
    bt.mute = bt_switch_cell(row, 1, btaudio_mute_get(), bt_mute_cb);
    lv_label_set_text(wl_note(row), "AF 0 while connected");
    row++;
    return row;
}

/* TCI server (tci_server.c): on/off, the port and the connected client */

static struct {
    lv_obj_t *sw;
    lv_obj_t *port;
    lv_obj_t *pill;
    lv_obj_t *client;
} wv;

static void wv_update() {
    const char *name = tci_server_client();
    char        port[24];

    /* The web configurator may change the setting while the page is open */
    if (tci_server_get_enabled() != lv_obj_has_state(wv.sw, LV_STATE_CHECKED)) {
        if (tci_server_get_enabled()) lv_obj_add_state(wv.sw, LV_STATE_CHECKED);
        else lv_obj_clear_state(wv.sw, LV_STATE_CHECKED);
    }
    snprintf(port, sizeof(port), "TCP %d", tci_server_port());
    lv_label_set_text(wv.port, port);
    wl_pill(wv.pill, name ? "connected" : NULL, theme_hex(TC_ok_bg), theme_hex(TC_ok));
    lv_label_set_text(wv.client, name ? name : "no client");
    lv_obj_set_style_text_color(wv.client, name ? TC(text) : TC(text_faint), 0);
}

static void wv_power_cb(lv_event_t *e) {
    tci_server_set_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    wv_update();
}

static uint8_t make_tci(uint8_t row) {
    lv_obj_t *obj;

    wl_row_label(row, "Server");
    wv.sw = bt_switch_cell(row, 1, tci_server_get_enabled(), wv_power_cb);
    row++;

    wl_row_label(row, "Port / Status");
    obj = lv_obj_create(grid);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 12, 0);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    wv.port = wl_label(obj, SF(22), theme_hex(TC_text_dim), 0);
    wv.pill = wl_pill_create(obj, 0);
    wv.client = wl_label(obj, SF(22), theme_hex(TC_text), 0);
    lv_obj_set_size(wv.client, 170, lv_font_get_line_height(SF(22)));   /* one line, then dots */
    lv_label_set_long_mode(wv.client, LV_LABEL_LONG_DOT);
    return row + 1;
}

/* Ethernet (wifi.cpp): the fields are a draft, typed one at a time with the keyboard;
 * Apply checks them all and saves. The draft lives across the page rebuilds the keyboard
 * makes, it is read again from the saved profile when the tab opens. */

static eth_cfg_t eth_draft;
static int       eth_kbd_field;
static struct {
    lv_obj_t *pill;
    lv_obj_t *iface;
    lv_obj_t *addr;
    lv_obj_t *gw;
    lv_obj_t *fields[ETH_FIELDS];
} eth;
static bool eth_shown;      /* the card is only there with an adapter plugged in */

static void eth_draft_load() {
    eth_info_t in;

    eth_get_info(&in);
    eth_draft = in.cfg;
}

static void eth_update() {
    eth_info_t in;

    if (!eth.pill) {
        return;
    }
    eth_get_info(&in);
    if (in.connected) {
        wl_pill(eth.pill, PILL_OK);
    } else {
        wl_pill(eth.pill, in.state, theme_hex(TC_chip), theme_hex(TC_text_value));
    }
    lv_label_set_text(eth.iface, in.iface);
    lv_label_set_text(eth.addr, in.addr[0] ? in.addr : "-");
    lv_label_set_text(eth.gw, in.gw[0] ? in.gw : "-");
}

static bool eth_kbd_ok() {
    char        val[32];
    const char *err;

    snprintf(val, sizeof(val), "%s", textarea_window_get());
    if (strlen(val) >= sizeof(eth_draft.field[0])) {
        err = "Too long";
    } else {
        err = eth_check_field(eth_kbd_field, val);
    }
    if (err) {
        msg_update_text_fmt("%s", err);
        return false;
    }
    strcpy(eth_draft.field[eth_kbd_field], val);
    return wl_kbd_close();
}

static void eth_field_cb(lv_event_t *e) {
    static const char *names[ETH_FIELDS] = {"IP address", "Netmask, 255.255.255.0 or /24", "Gateway (optional)", "DNS (optional)"};

    eth_kbd_field = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    wl_kbd = true;
    lv_group_remove_all_objs(keyboard_group);
    textarea_window_open(eth_kbd_ok, wl_kbd_close);
    lv_textarea_set_max_length(textarea_window_text(), 15);
    lv_textarea_set_accepted_chars(textarea_window_text(), "0123456789./");     /* digits: the 1# key */
    lv_textarea_set_placeholder_text(textarea_window_text(), names[eth_kbd_field]);
    textarea_window_set(eth_draft.field[eth_kbd_field]);
}

static void eth_mode_cb(lv_event_t *e) {
    eth_draft.dhcp = lv_dropdown_get_selected(lv_event_get_target(e)) == 0;
}

static void eth_apply_cb(lv_event_t *e) {
    eth_cfg_t   cfg = eth_draft;
    const char *err = eth_check(&cfg);

    if (err) {
        msg_update_text_fmt("%s", err);
        return;
    }
    eth_draft = cfg;
    for (int i = 0; i < ETH_FIELDS; i++) {
        lv_label_set_text(lv_obj_get_child(eth.fields[i], 0), cfg.field[i][0] ? cfg.field[i] : "-");
    }
    eth_apply(&cfg);
    msg_update_text_fmt("Ethernet settings saved");
}

static uint8_t make_ethernet(uint8_t row) {
    static const char *labels[ETH_FIELDS] = {"IP address", "Netmask", "Gateway", "DNS"};
    lv_obj_t *obj;

    wl_row_label(row, "Status");
    obj = lv_obj_create(grid);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 12, 0);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    eth.pill = wl_pill_create(obj, 0);
    eth.iface = wl_label(obj, SF(22), theme_hex(TC_text), 0);
    row++;

    for (int i = 0; i < 2; i++) {
        obj = wl_label(grid, SF(22), theme_hex(TC_text_dim), 0);
        lv_label_set_text(obj, i ? "Gateway in use" : "Address in use");
        lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1);
        obj = wl_label(grid, SF(22), theme_hex(TC_text_dim), 0);
        lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
        *(i ? &eth.gw : &eth.addr) = obj;
        row++;
    }

    wl_row_label(row, "Mode");
    obj = lv_dropdown_create(grid);
    dialog_item(&dialog, obj);
    styles_dropdown_list(lv_dropdown_get_list(obj));
    lv_dropdown_set_options(obj, " DHCP \n Static ");
    lv_dropdown_set_symbol(obj, NULL);
    lv_dropdown_set_selected(obj, eth_draft.dhcp ? 0 : 1);
    lv_obj_add_event_cb(obj, eth_mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_size(obj, SMALL_3, ROW_H);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 3, LV_GRID_ALIGN_CENTER, row, 1);
    row++;

    for (int i = 0; i < ETH_FIELDS; i++) {
        wl_row_label(row, labels[i]);
        obj = wl_button(row, eth_draft.field[i][0] ? eth_draft.field[i] : "-", eth_field_cb);
        lv_obj_set_size(obj, SMALL_4, ROW_H - 4);
        lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 4, LV_GRID_ALIGN_CENTER, row, 1);
        lv_obj_set_user_data(obj, (void *)(intptr_t)i);
        eth.fields[i] = obj;
        row++;
    }

    obj = wl_label(grid, SF(22), theme_hex(TC_text), 0);   /* own colour: no leader dots */
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, row, 1);
    wl_button(row, "Apply", eth_apply_cb);
    return row + 1;
}

/* An adapter plugged in or pulled out: the page again, with or without the card */
static void eth_rebuild(void *arg) {
    if (!wl_timer || wl_kbd || wl_menu) {
        return;     /* page gone, or busy: the keyboard and menu build it again when they close */
    }
    eth_draft_load();
    make_wireless_page();
}

static void wl_timer_cb(lv_timer_t *t) {
    eth_info_t in;

    eth_get_info(&in);
    if (in.present != eth_shown && !wl_kbd && !wl_menu) {
        lv_async_call(eth_rebuild, NULL);
    }
    wifi_update();
    eth_update();
}

/* Objects die with the page: late callbacks must not touch them */
static void bt_forget_objs() {
    bt.pill = bt.state = bt.addr = bt.cat = NULL;
    bt.scan_btn = bt.scan_note = bt.rx = bt.vol = bt.mute = NULL;
}

static void wl_delete_cb(lv_event_t *e) {
    wl_gen++;
    lv_timer_del(wl_timer);
    wl_timer = NULL;
    wl_menu_back = NULL;
    wl_menu_close();
    if (wl_kbd) {
        wl_kbd = false;
        textarea_window_close();
    }
}

static lv_obj_t *focus_label;   /* the focused row's label, bright */

static void focus_row_label(lv_obj_t *obj) {
    if (focus_label) {
        lv_obj_set_style_text_color(focus_label, TC(text_value), 0);
        focus_label = NULL;
    }
    while (obj && lv_obj_get_parent(obj) && lv_obj_get_style_layout(lv_obj_get_parent(obj), 0) != LV_LAYOUT_GRID) {
        obj = lv_obj_get_parent(obj);
    }
    if (!obj || !lv_obj_get_parent(obj)) return;

    lv_obj_t *g = lv_obj_get_parent(obj);
    int       r = lv_obj_get_style_grid_cell_row_pos(obj, 0);

    for (uint32_t i = 0; i < lv_obj_get_child_cnt(g); i++) {
        lv_obj_t *c = lv_obj_get_child(g, i);

        if (lv_obj_has_flag(c, ROW_LABEL) && lv_obj_get_style_grid_cell_row_pos(c, 0) == r) {
            focus_label = c;
            lv_obj_set_style_text_color(c, TC(text), 0);
            break;
        }
    }
}

/* MFK focus: bring the whole row into view, with a margin (the default
 * scroll-on-focus left rows at the bottom edge half hidden) */
static void focus_cb(lv_group_t *g) {
    lv_obj_t *obj = lv_group_get_focused(g);

    focus_row_label(obj);
    if (!page || !obj || !lv_obj_has_flag(page, LV_OBJ_FLAG_SCROLLABLE)) {
        return;
    }
    lv_area_t  a, p;
    lv_coord_t margin = 24;

    lv_obj_update_layout(page);

    /* Rows without controls above the first or below the last one (status
     * lines, card ends) would never come into view: show the page ends */
    lv_obj_t **first = (lv_obj_t **)_lv_ll_get_head(&g->obj_ll);
    lv_obj_t **last  = (lv_obj_t **)_lv_ll_get_tail(&g->obj_ll);

    if (first && *first == obj) {   /* also the only one while the page is built */
        lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
        return;
    }
    if (last && *last == obj) {
        lv_coord_t below = lv_obj_get_scroll_bottom(page);

        if (below > 0) lv_obj_scroll_by(page, 0, -below, LV_ANIM_OFF);
        return;
    }
    lv_obj_get_coords(obj, &a);
    lv_obj_get_coords(page, &p);
    if (a.y2 + margin > p.y2) {
        lv_obj_scroll_by(page, 0, p.y2 - (a.y2 + margin), LV_ANIM_OFF);
    } else if (a.y1 - margin < p.y1) {
        lv_obj_scroll_by(page, 0, p.y1 - (a.y1 - margin), LV_ANIM_OFF);
    }
}

static void grid_delete() {
    if (page) {
        lv_group_focus_cb_t cb = lv_group_get_focus_cb(keyboard_group);

        observers.clear();     /* their sliders go with the page */
        lv_group_set_editing(keyboard_group, false);
        /* Not while the rows go: the focus steps back over them and focus_cb() would colour
         * a row label already deleted (Settings opened again on the theme, then a tab) */
        lv_group_set_focus_cb(keyboard_group, NULL);
        lv_obj_del(page);
        lv_group_set_focus_cb(keyboard_group, cb);
        page = NULL;
        grid = NULL;
        focus_label = NULL;
    }
}

/* The page again from nothing */
static void grid_create() {
    grid_delete();
    card_n = 0;
    page = lv_obj_create(dialog.obj);
    lv_obj_set_size(page, 796 - RAIL_W - 14, DIALOG_H - 8);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_text_color(page, TC(text), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 6, 0);
    lv_obj_set_style_pad_row(page, 8, 0);
    lv_obj_set_style_pad_bottom(page, 40, 0);   /* room under the last row */
    lv_obj_set_style_text_font(page, SF(22), 0);  /* dialogs use 36 */

    lv_obj_align(page, LV_ALIGN_RIGHT_MID, -4, 0);   /* right of the rail */
}

/* Station: callsign and QTH locator, edited with the on-screen keyboard */

#define ST_CALLSIGN 1
#define ST_QTH      2

static int st_kbd;  /* keyboard open for ST_CALLSIGN / ST_QTH */

/* The keyboard gets all the keys: the page is built again when it closes */
static bool st_kbd_close() {
    st_kbd = 0;
    textarea_window_close();
    make_general_page();
    return true;
}

static bool st_kbd_ok() {
    const char *val = textarea_window_get();
    bool        ok  = st_kbd == ST_CALLSIGN ? station_callsign_save(val) : station_qth_save(val);

    return ok && st_kbd_close();
}

static void st_kbd_open_cb(lv_event_t *e) {
    st_kbd = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    lv_group_remove_all_objs(keyboard_group);
    textarea_window_open(st_kbd_ok, st_kbd_close);
    if (st_kbd == ST_CALLSIGN) {
        station_callsign_edit(textarea_window_text());
    } else {
        station_qth_edit(textarea_window_text());
    }
}

static lv_timer_t *wv_timer;   /* the TCI client status on General */

static void wv_timer_cb(lv_timer_t *t) {
    wv_update();
}

static void st_delete_cb(lv_event_t *e) {
    if (st_kbd || wl_kbd) {
        st_kbd = 0;
        wl_kbd = false;
        textarea_window_close();
    }
    if (wv_timer) {
        lv_timer_del(wv_timer);
        wv_timer = NULL;
    }
}

static uint8_t make_station_row(uint8_t row, const char *label, const char *val, int which) {
    wl_row_label(row, label);

    lv_obj_t *obj = wl_button(row, val[0] ? val : "-", st_kbd_open_cb);
    lv_obj_set_size(obj, 150, ROW_H - 4);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_CENTER, row, 1);
    lv_obj_set_user_data(obj, (void *)(intptr_t)which);
    return row + 1;
}

static uint8_t make_station(uint8_t row) {
    row = make_station_row(row, "Callsign", params.callsign.x, ST_CALLSIGN);
    row = make_station_row(row, "QTH locator", params.qth.x, ST_QTH);
    /* A and B each on their own band: the VFO key brings the other one's band back */
    row = make_param_switch(row, "Independent VFOs", cfg_sm.p_indep_vfo);
    /* The knob turned into another band plan segment: CW there, SSB in the phone part */
    row = make_param_switch(row, "Mode from band plan", cfg_sm.p_mode_from_plan);
    lv_obj_add_event_cb(grid, st_delete_cb, LV_EVENT_DELETE, NULL);
    return row;
}

/* Audio: microphone, VOX, AGC */

static param_slider_t hmic_slider      = {&cfg_sm.p_hmic, 0, 50, 1, "%i"};
static param_slider_t imic_slider      = {&cfg_sm.p_imic, 0, 50, 1, "%i"};
static param_slider_t vox_gain_slider  = {&cfg_sm.p_vox_gain, 0, 100, 1, "%i"};
static param_slider_t vox_ag_slider    = {&cfg_sm.p_vox_ag, 0, 100, 1, "%i"};
static param_slider_t vox_delay_slider = {&cfg_sm.p_vox_delay, 100, 2000, 50, "%i ms"};
static param_slider_t agc_knee_slider  = {&cfg_sm.p_agc_knee, -100, 0, 1, "%i dB"};
static param_slider_t agc_slope_slider = {&cfg_sm.p_agc_slope, 0, 10, 1, "%i dB"};
static param_slider_t smeter_cal_slider = {&cfg_sm.p_band_smeter_cal, -40, 40, 1, "%+i dB"};
static param_slider_t snap_hold_slider = {&cfg_sm.p_spot_snap_hold, 0, 5, 1, "%i"};
static param_slider_t snap_slow_slider = {&cfg_sm.p_spot_snap_slow, 1, 5, 1, "%i"};
static param_slider_t accel_onset_slider = {&cfg_sm.p_freq_accel_onset, 1, 5, 1, "%i"};
static param_slider_t wf_speed_slider = {&cfg_sm.p_waterfall_speed, 1, 5, 1, "%i"};
static param_slider_t spectrum_height_slider = {&cfg_sm.p_spectrum_height, 60, 160, 4, "%i px"};

/* S-meter cal, when its editing starts: what it is for, before the reading drifts from the truth.
 * Built as the cluster alert (theme roles, popup radius, sony_18 title), framed in the error colour */
static lv_obj_t   *smeter_warn;
static lv_timer_t *smeter_warn_timer;

static void smeter_warn_close(lv_timer_t *t) {
    if (smeter_warn) {
        lv_obj_del(smeter_warn);
        smeter_warn = NULL;
    }
    smeter_warn_timer = NULL;
}

extern "C" void smeter_warn_show(void) {   /* not static: the UI harness shows it in every theme */
    lv_obj_t *l;

    if (smeter_warn) {
        return;
    }
    smeter_warn = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(smeter_warn);
    lv_obj_set_size(smeter_warn, 560, LV_SIZE_CONTENT);
    lv_obj_align(smeter_warn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(smeter_warn, TC(popup), 0);
    lv_obj_set_style_bg_opa(smeter_warn, 240, 0);
    lv_obj_set_style_border_color(smeter_warn, TC(err), 0);
    lv_obj_set_style_border_width(smeter_warn, 3, 0);
    lv_obj_set_style_radius(smeter_warn, theme_shape.radius_popup, 0);
    lv_obj_set_style_pad_all(smeter_warn, 14, 0);
    lv_obj_set_style_pad_row(smeter_warn, 8, 0);
    lv_obj_set_flex_flow(smeter_warn, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(smeter_warn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    l = lv_label_create(smeter_warn);
    lv_obj_set_style_text_font(l, &sony_18, 0);
    lv_obj_set_style_text_color(l, TC(err), 0);
    lv_label_set_text(l, "S-METER CALIBRATION");

    l = lv_label_create(smeter_warn);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_font(l, &sony_24, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "For this band only. Feed -73 dBm and set the reading to S9 on each "
                         "band you use. -22 dB matches an IC-7300.");

    smeter_warn_timer = lv_timer_create(smeter_warn_close, 5000, NULL);
    lv_timer_set_repeat_count(smeter_warn_timer, 1);
}

static void smeter_cal_warn_cb(lv_event_t *e) {
    lv_obj_t   *obj = lv_event_get_target(e);
    lv_indev_t *in  = lv_indev_get_act();
    bool        pointer = in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER;
    lv_group_t *g   = (lv_group_t *)lv_obj_get_group(obj);

    /* Touch: pressing it changes it. Knob: the press that starts editing focuses it in edit
     * mode (it sends no PRESSED; the press that ends editing does) */
    if ((lv_event_get_code(e) == LV_EVENT_PRESSED && pointer) ||
        (lv_event_get_code(e) == LV_EVENT_FOCUSED && g && lv_group_get_editing(g))) {
        smeter_warn_show();
    }
}

static uint8_t make_param_slider(uint8_t row, const char *label, param_slider_t *p) {
    lv_obj_t *obj;

    wl_row_label(row, label);
    /* As the other sliders: the right half, value at the right */
    obj = param_slider(row_cell(row, 4, 3, SMALL_3), SMALL_3 - 134, p);
    if (p == &smeter_cal_slider) {
        lv_obj_add_event_cb(obj, smeter_cal_warn_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(obj, smeter_cal_warn_cb, LV_EVENT_FOCUSED, NULL);
    }

    return row + 1;
}

/* The microphone input: Audio > MICROPHONE (p_mic) and the walkie's own (p_walkie_mic).
 * Same order as x6100_mic_builtin, x6100_mic_handle, x6100_mic_auto. */
static uint8_t make_mic(uint8_t row, const char *label, ParamInt &param) {
    wl_row_label(row, label);
    row_choice(row, 4, 3, SMALL_3, " Built-in \n Hand \n Auto ", param.get(), int_choice_cb, &param);
    return row + 1;
}

/* Time zone: POSIX TZ strings, applied at once (see clock_tz_set) */

static const struct { const char *label; const char *posix; } zones[] = {
    /* UTC, then west to east; the POSIX strings must differ (the current one is found by its string) */
    { "UTC",                              "UTC0" },
    { "Pacific/Honolulu (HST)",           "HST10" },
    { "America/Anchorage (AKST/AKDT)",    "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles (PST/PDT)",    "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Denver (MST/MDT)",         "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Chicago (CST/CDT)",        "CST6CDT,M3.2.0,M11.1.0" },
    { "America/New_York (EST/EDT)",       "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Sao_Paulo (BRT)",          "BRT3" },
    { "America/Buenos_Aires (ART)",       "ART3" },
    { "Europe/London (GMT/BST)",          "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon, Canary (WET/WEST)", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Europe/Rome (CET/CEST)",           "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Athens (EET/EEST)",         "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Africa/Johannesburg (SAST)",       "SAST-2" },
    { "Europe/Moscow (MSK)",              "MSK-3" },
    { "Asia/Kolkata (IST)",               "IST-5:30" },
    { "Asia/Singapore (SGT)",             "SGT-8" },
    { "Asia/Shanghai (CST)",              "CST-8" },
    { "Australia/Perth (AWST)",           "AWST-8" },
    { "Asia/Seoul (KST)",                 "KST-9" },
    { "Asia/Tokyo (JST)",                 "JST-9" },
    { "Australia/Brisbane (AEST)",        "AEST-10" },
    { "Australia/Sydney (AEST/AEDT)",     "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Pacific/Auckland (NZST/NZDT)",     "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define ZONES_NUM   (sizeof(zones) / sizeof(zones[0]))

static void zone_update_cb(lv_event_t *e) {
    time_t now = time(NULL);

    clock_tz_set(zones[lv_dropdown_get_selected(lv_event_get_target(e))].posix);
    localtime_r(&now, &dt_tm);
    dt_show();
}

static uint8_t make_zone(uint8_t row) {
    lv_obj_t    *obj;
    std::string  options;
    const char  *tz = getenv("TZ");
    uint16_t     sel = 0;

    for (size_t i = 0; i < ZONES_NUM; i++) {
        options += std::string(i ? "\n " : " ") + zones[i].label + " ";
        if (tz && !strcmp(tz, zones[i].posix)) {
            sel = i;
        }
    }
    wl_row_label(row, "Zone");
    obj = row_choice(row, 1, 3, 270, options.c_str(), sel, zone_update_cb, NULL);
    lv_obj_set_style_text_font(obj, &sony_22, 0);   /* the long zone names fit at this size only */
    lv_obj_set_style_text_font(lv_dropdown_get_list(obj), SF(22), 0);

    return row + 1;
}

static void make_general_page() {
    time_t  now = time(NULL);
    uint8_t row;

    grid_create();
    dt_tm = *localtime(&now);

    row = card_begin("STATION", col_station);
    row = make_station(row);
    card_end(row);

    row = card_begin("DATE / TIME", col_datetime);
    row = make_dt_row(row, "Date", 0);
    row = make_dt_row(row, "Time", 3);
    row = make_zone(row);
    card_end(row);

    row = card_begin("CONTROLS");
    row = make_freq_accel(row);
    row = make_param_slider(row, "Accel. onset", &accel_onset_slider);
    row = make_charger(row);
    row = make_param_slider(row, "Per band S-meter cal", &smeter_cal_slider);
    /* The Walkie Talkie portrait screen: on at once (walkie.c follows the setting), Settings closes */
    row = make_param_switch(row, "Walkie Talkie", cfg_sm.p_walkie);
    card_end(row);

    row = card_begin("TRANSVERTERS");
    row = make_transverters(row);
    card_end(row);

    row = card_begin("TCI SERVER");
    row = make_tci(row);
    card_end(row);
    wv_update();
    wv_timer = lv_timer_create(wv_timer_cb, 1000, NULL);   /* deleted by st_delete_cb */
}

/* The Walkie Talkie screen: on (Settings closes, the portrait screen comes up), its knob, its mic */
static void make_walkie_page() {
    grid_create();

    uint8_t row = card_begin("WALKIE TALKIE");
    row = make_param_switch(row, "Walkie Talkie", cfg_sm.p_walkie);
    row = make_param_switch(row, "Main knob steps channels", cfg_sm.p_walkie_mem);
    row = make_mic(row, "Microphone", cfg_sm.p_walkie_mic);
    card_end(row);
}

/* USB paddle (paddle_keyer.c): what is plugged in, the codes of each source, Learn, Swap.
 * The keyer's mode, speed and ratio stay on the KEY buttons in CW. */

#define PD_TICK_MS      100
#define PD_LEARN_TICKS  100     /* 10 s for each press */

static struct {
    lv_timer_t *timer;
    lv_obj_t   *connected;
    lv_obj_t   *map[PADDLE_SRC_N];
    lv_obj_t   *learn_note;
    int         step;           /* Learn: 0 off, 1 waiting for DIT, 2 for DAH */
    int         dit;            /* (src << 16 | code) of the DIT press */
    int         ticks;
} pd;

static ParamInt *pd_param(int src) {   /* paddle_src_t order */
    ParamInt *p[] = {&cfg_sm.p_paddle_kbd, &cfg_sm.p_paddle_btn, &cfg_sm.p_paddle_midi, &cfg_sm.p_paddle_ser};

    return p[src];
}

static const char * pd_code_name(int src, int code, char *buf) {
    static const char *rows[] = {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./"};
    static const int   first[] = {KEY_1, KEY_Q, KEY_A, KEY_Z};

    switch (src) {
        case PADDLE_SRC_KBD:
            switch (code) {
                case KEY_LEFTCTRL:   return "Left Ctrl";
                case KEY_RIGHTCTRL:  return "Right Ctrl";
                case KEY_LEFTSHIFT:  return "Left Shift";
                case KEY_RIGHTSHIFT: return "Right Shift";
                case KEY_LEFTALT:    return "Left Alt";
                case KEY_RIGHTALT:   return "Right Alt";
                case KEY_SPACE:      return "Space";
            }
            for (int r = 0; r < 4; r++) {
                if (code >= first[r] && code < first[r] + (int)strlen(rows[r])) {
                    sprintf(buf, "%c", rows[r][code - first[r]]);
                    return buf;
                }
            }
            sprintf(buf, "key %i", code);
            return buf;
        case PADDLE_SRC_BTN:
            switch (code) {
                case BTN_LEFT:   return "left";
                case BTN_RIGHT:  return "right";
                case BTN_MIDDLE: return "middle";
            }
            sprintf(buf, "button %i", code);
            return buf;
        case PADDLE_SRC_MIDI:
            sprintf(buf, "%i", code);
            return buf;
        default:
            switch (code) {
                case TIOCM_CTS: return "CTS";
                case TIOCM_DSR: return "DSR";
                case TIOCM_CD:  return "DCD";
                case TIOCM_RI:  return "RI";
            }
            sprintf(buf, "line %i", code);
            return buf;
    }
}

/* Dit and dah codes of the map as they key now: KEY mode Auto-R and Swap flip them */
static bool pd_flip() {
    return (cfg_sm.p_key_mode.get() == x6100_key_auto_right) != (bool)cfg_sm.p_paddle_swap.get();
}

static void pd_update() {
    static const char *src_name[] = {"keyboard", "mouse buttons", "MIDI", ""};
    const char        *tty;
    unsigned           s = paddle_keyer_sources(&tty);
    char               text[96] = "";
    char               a[16], b[16];

    for (int i = 0; i < PADDLE_SRC_N; i++) {
        if (s & 1u << i) {
            snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s", text[0] ? ", " : "",
                     i == PADDLE_SRC_SER ? tty : src_name[i]);
        }
        int map = pd_param(i)->get();
        int lo = map & 0xffff, hi = map >> 16;

        if (pd_flip()) std::swap(lo, hi);
        lv_label_set_text_fmt(pd.map[i], "dit %s, dah %s", pd_code_name(i, lo, a), pd_code_name(i, hi, b));
    }
    lv_label_set_text(pd.connected, text[0] ? text : "nothing");
}

static void pd_learn_end(const char *note) {
    pd.step = 0;
    paddle_keyer_learn(false);
    lv_label_set_text(pd.learn_note, note);
}

static void pd_timer_cb(lv_timer_t *t) {
    static int tick;

    if (pd.step) {
        int got = paddle_keyer_learned();

        if (got >= 0 && pd.step == 1) {
            pd.dit   = got;
            pd.step  = 2;
            pd.ticks = 0;
            lv_label_set_text(pd.learn_note, "Press DAH");
        } else if (got >= 0) {
            int src = got >> 16;

            if (src != pd.dit >> 16 || got == pd.dit) {
                pd_learn_end("Not a second contact: Learn again");
            } else {
                pd_param(src)->set(paddle_learn_map(pd.dit & 0xffff, got & 0xffff, pd_flip()));
                pd_learn_end("Saved");
                pd_update();
            }
        } else if (++pd.ticks >= PD_LEARN_TICKS) {
            pd_learn_end("Nothing came in");
        }
    }
    if (++tick >= 1000 / PD_TICK_MS) {   /* the Serial port switch and anything plugged in */
        tick = 0;
        paddle_keyer_scan();
        pd_update();
    }
}

static void pd_learn_cb(lv_event_t *e) {
    pd.step  = 1;
    pd.ticks = 0;
    paddle_keyer_learn(true);
    lv_label_set_text(pd.learn_note, "Press DIT");
}

static void pd_delete_cb(lv_event_t *e) {
    if (pd.step) {
        pd.step = 0;
        paddle_keyer_learn(false);
    }
    if (pd.timer) {
        lv_timer_del(pd.timer);
        pd.timer = NULL;
    }
}

static lv_obj_t * pd_value(uint8_t row, const char *label) {
    wl_row_label(row, label);

    lv_obj_t *obj = wl_label(grid, SF(22), theme_hex(TC_text), 0);

    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_END, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    return obj;
}

static uint8_t make_paddle(uint8_t row) {
    pd.connected = pd_value(row++, "Connected");
    pd.map[PADDLE_SRC_KBD]  = pd_value(row++, "Keys ([ ] too)");
    pd.map[PADDLE_SRC_BTN]  = pd_value(row++, "Mouse buttons");
    pd.map[PADDLE_SRC_MIDI] = pd_value(row++, "MIDI notes");
    pd.map[PADDLE_SRC_SER]  = pd_value(row++, "Serial lines");

    wl_row_label(row, "Learn dit and dah");
    wl_button(row, "Start", pd_learn_cb);
    pd.learn_note = wl_note(row++);

    row = make_param_switch(row, "Swap dit and dah", cfg_sm.p_paddle_swap);   /* the timer shows the map again */
    row = make_param_switch(row, "Serial port (ttyUSB, ttyACM)", cfg_sm.p_paddle_serial);

    lv_label_set_text(pd_value(row++, "Mode, speed, ratio"), "KEY buttons in CW");

    pd.step = 0;
    pd_update();
    pd.timer = lv_timer_create(pd_timer_cb, PD_TICK_MS, NULL);
    lv_obj_add_event_cb(grid, pd_delete_cb, LV_EVENT_DELETE, NULL);
    return row;
}

/* What the front keys (long press) and the hand mic keys do, the USB paddle */
static void make_keys_page() {
    uint8_t row;

    grid_create();
    row = card_begin("FRONT KEYS, LONG PRESS");
    row = make_key_rows(row, 0, KEY_ROWS_FRONT);
    card_end(row);

    row = card_begin("HAND MIC KEYS");
    row = make_key_rows(row, KEY_ROWS_FRONT, sizeof(key_rows) / sizeof(key_rows[0]));
    card_end(row);

    row = card_begin("PADDLE");
    row = make_paddle(row);
    card_end(row);
}

/* Cluster: which spots the app, the column and the waterfall show. Saved in
 * cluster.conf next to the web configurator's filters and alerts. */

#define CF_MODES 4

static const uint8_t cf_mode_bit[CF_MODES]  = { CLUSTER_FM_CW, CLUSTER_FM_SSB, CLUSTER_FM_DIG, CLUSTER_FM_AMFM };
static const char   *cf_mode_name[CF_MODES] = { "CW", "SSB", "DIG", "AM/FM" };

static struct {
    lv_obj_t *mode[CF_MODES];
    lv_obj_t *band[16];
    int       band_n;
    lv_obj_t *act[CLUSTER_ACT_COUNT];
} cf;

static void cf_save() {
    uint8_t  modes = 0;
    uint16_t bands = 0;
    uint8_t  act   = 0;
    int      on    = 0;

    for (int i = 0; i < CF_MODES; i++) {
        if (lv_obj_has_state(cf.mode[i], LV_STATE_CHECKED)) modes |= cf_mode_bit[i];
    }
    for (int b = 0; b < cf.band_n; b++) {
        if (lv_obj_has_state(cf.band[b], LV_STATE_CHECKED)) {
            bands |= 1u << b;
            on++;
        }
    }
    if (on == cf.band_n) bands = 0;     /* all of them: no filter */
    for (int i = 0; i < CLUSTER_ACT_COUNT; i++) {
        if (lv_obj_has_state(cf.act[i], LV_STATE_CHECKED)) act |= 1u << i;
    }
    cluster_filter_save(modes, bands, act);
}

static void cf_toggle_cb(lv_event_t *e) {
    lv_obj_t  *obj  = lv_event_get_target(e);
    lv_obj_t **list = (lv_obj_t **)lv_event_get_user_data(e);
    int        n    = list == cf.mode ? CF_MODES : list == cf.act ? CLUSTER_ACT_COUNT : cf.band_n;
    bool       any  = list == cf.act;  /* activity: none lit is all spots */

    for (int i = 0; i < n; i++) any |= lv_obj_has_state(list[i], LV_STATE_CHECKED);
    if (!any) {
        lv_obj_add_state(obj, LV_STATE_CHECKED);    /* one at least: none would hide every spot */
        return;
    }
    cf_save();
}

static lv_obj_t * cf_toggle(uint8_t row, int col, const char *text, bool on, lv_obj_t **list) {
    lv_obj_t *obj = lv_btn_create(grid);

    lv_obj_remove_style_all(obj);
    dialog_item(&dialog, obj);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, TC(control_border), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(obj, TC(toggle_on), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(obj, TC(text), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(obj, TC(toggle_focus), LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(obj, TC(focus_text), LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_set_size(obj, SMALL_1, ROW_H - 4);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, col, 1, LV_GRID_ALIGN_CENTER, row, 1);
    if (on) lv_obj_add_state(obj, LV_STATE_CHECKED);
    lv_obj_add_event_cb(obj, cf_toggle_cb, LV_EVENT_VALUE_CHANGED, list);

    lv_obj_t *label = lv_label_create(obj);
    lv_obj_set_style_text_font(label, theme_shape.vision ? &sony_14 : &sony_18, 0);   /* VISION scales it to 20 */
    if (theme_shape.vision) lv_obj_set_style_text_letter_space(label, -2, 0);   /* AM/FM fits only closer together */
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return obj;
}

static void make_cluster_page() {
    const cluster_filter_t *f = cluster_filter();
    uint8_t                 row;
    lv_obj_t               *obj;

    grid_create();
    row = card_begin("CLUSTER FILTERS");

    wl_row_label(row, "Modes");
    for (int i = 0; i < CF_MODES; i++) {
        cf.mode[i] = cf_toggle(row, 3 + i, cf_mode_name[i], f->modes & cf_mode_bit[i], cf.mode);
    }
    row++;

    /* Two rows of bands, the second right-aligned */
    cf.band_n = 0;
    while (cf.band_n < 16 && cluster_band_name(cf.band_n)[0]) cf.band_n++;
    int first = (cf.band_n + 1) / 2;

    wl_row_label(row, "Bands (m)");
    for (int b = 0; b < cf.band_n; b++) {
        char name[8];
        int  r   = b < first ? row : row + 1;
        int  col = b < first ? 1 + b + (6 - first) : 1 + (b - first) + (6 - (cf.band_n - first));

        snprintf(name, sizeof(name), "%d", atoi(cluster_band_name(b)));
        cf.band[b] = cf_toggle(r, col, name, !f->bands || (f->bands & (1u << b)), cf.band);
    }
    row += 2;

    /* Two on a row, two columns each: WWBOTA is wider than a mode */
    wl_row_label(row, "Activity");
    for (int i = 0; i < CLUSTER_ACT_COUNT; i++) {
        cf.act[i] = cf_toggle(row + i / 2, 3 + i % 2 * 2, cluster_act_names[i], f->activity & (1u << i), cf.act);
        lv_obj_set_width(cf.act[i], SMALL_1 * 2 + SMALL_PAD);
        lv_obj_set_grid_cell(cf.act[i], LV_GRID_ALIGN_START, 3 + i % 2 * 2, 2, LV_GRID_ALIGN_CENTER, row + i / 2, 1);
    }
    row += 2;

    obj = wl_label(grid, SF(18), theme_hex(TC_text_dim), 0);
    lv_label_set_text(obj, "For the app, the column and the waterfall. Alerts always show.\n"
                           "Activity: none lit = all spots. Max age, spotters, alerts: web Cluster page.");
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 0, 7, LV_GRID_ALIGN_CENTER, row, 1);
    row_dsc[row] = LV_GRID_CONTENT;
    row++;

    card_end(row);

    /* The same settings as the Cluster app's Order and Band / All and the MFK items */
    row = card_begin("CLUSTER VIEW");
    row = make_param_switch(row, "Newest first", cfg_sm.p_cluster_order);
    row = make_param_switch(row, "All bands", cfg_sm.p_cluster_all_bands);
    row = make_param_slider(row, "Spot snap hold", &snap_hold_slider);
    row = make_param_slider(row, "Spot snap speed", &snap_slow_slider);
    card_end(row);

    /* The other way: what this radio decodes, reported to pskreporter.info */
    row = card_begin("PSK REPORTER");
    row = make_param_switch(row, "Report decodes", cfg_sm.p_pskreporter);   /* FT8 FT4 FT2 JS8 PSK31 PSK63 Olivia */
    card_end(row);
}

/* SWR protection: the most SWR a transmission keeps (tenths), 1.0 off; steps of 0.5 */
static void swr_protect_cb(lv_event_t *e) {
    lv_obj_t *obj   = lv_event_get_target(e);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(obj);
    int32_t   val   = lv_slider_get_value(obj) * 5;

    if (val <= 10) {
        lv_label_set_text(label, "Off");
    } else {
        lv_label_set_text_fmt(label, "%i.%i", val / 10, val % 10);
    }
    cfg_sm.p_swr_protect.set(val);
}

static uint8_t make_swr_protect(uint8_t row) {
    int32_t   val = cfg_sm.p_swr_protect.get();
    lv_obj_t *obj;

    wl_row_label(row, "SWR protection");
    obj = row_slider(row_cell(row, 4, 3, SMALL_3), val / 5, 2, 10, SMALL_3 - 120, swr_protect_cb, NULL, "%i", val);
    lv_event_send(obj, LV_EVENT_VALUE_CHANGED, NULL);   /* the label as 2.5 / Off */

    return row + 1;
}

/* TX CAL: the transmitter's calibration and CESSB, per band where the base keeps them per band */
static void make_txcal_page() {
    x6100_base_ver_t base = x6100_control_get_base_ver();
    uint8_t          row;

    grid_create();
    row = card_begin("TX CALIBRATION");
    row = make_iq_offsets(row);
    if (base.rev >= 8) {
        row = make_band_gain(row);
    }
    if (base.rev >= 3) {
        row = make_codec_gain(row);
    }
    card_end(row);

    if (base.rev >= 12) {
        row = card_begin("CESSB");
        row = make_cessb(row);
        card_end(row);
    }

    row = card_begin("TX PROTECTION");
    row = make_swr_protect(row);
    card_end(row);
}

static void make_display_page() {
    uint8_t row;

    grid_create();
    row = card_begin("BACKLIGHT");
    row = make_backlight(row);
    card_end(row);

    row = card_begin("SPECTRUM");
    row = make_spectrum_show(row);
    if (cfg_sm.p_spectrum_show.get()) {
        row = make_spectrum_fill_peak(row);
        row = make_param_switch(row, "Fill gradient", cfg_sm.p_spectrum_fill_gradient);
        /* Filter passband on the spectrum and waterfall: the theme's colour or a preset (theme_filter_color()) */
        row = make_param_choice(row, "Filter colour",
                                " Theme \n Blue \n Cyan \n Green \n Yellow \n Orange \n Red \n Violet \n White ",
                                cfg_sm.p_filter_color);
        row = make_param_slider(row, "Height", &spectrum_height_slider);   /* live, at each step */
    }
    card_end(row);

    row = card_begin("WATERFALL");
    /* Centered: the VFO stays in the middle. Fixed: the span stays put, the VFO line moves */
    row = make_param_choice(row, "Mode", " Centered \n Fixed ", cfg_sm.p_wf_mode);
    row = make_auto_offset(row);
    row = make_grid_min_max(row);
    row = make_waterfall_line_zoom(row);
    wl_row_label(row, "Smooth scroll");
    bool_switch(row++, 4, 3, SMALL_3, &params.waterfall_smooth_scroll);
    row = make_param_slider(row, "Speed", &wf_speed_slider);
    card_end(row);

    /* Overlays: shown with or without the spectrum */
    row = card_begin("MAIN SCREEN");
    row = make_param_switch(row, "Band plan", cfg_sm.p_band_plan);
    row = make_param_switch(row, "Cluster spots", cfg_sm.p_cluster_spots_wf);
    row = make_param_switch(row, "Cluster column", cfg_sm.p_cluster_column);
    row = make_param_switch(row, "Broadcast names", cfg_sm.p_broadcast_name);
    row = make_param_choice(row, "S-meter type", " Bars \n Needle ", cfg_sm.p_smeter_needle);
    /* Theme: as the theme draws it. Scale: green, yellow, red along the scale. Level: the lit bars all
     * in the colour of the signal */
    row = make_param_choice(row, "S-meter colours", " Theme \n Scale \n Level ", cfg_sm.p_smeter_color);
    row = make_param_switch(row, "Needle lamp", cfg_sm.p_smeter_lamp);
    card_end(row);

    row = card_begin("INTERFACE");
    row = make_mag(row);
    wl_row_label(row, "Icon menus");   /* APP, Modes and the rail as icons: the next time they open */
    bool_switch(row++, 1, 3, SMALL_3, &params.icon_menus);
    row = make_param_switch(row, "Knob info", cfg_sm.p_knob_info);
    row = make_theme(row);
    row = make_font(row);
    /* CW / RTTY decoder text: Large or Compact (more lines), from SP2KMO's upstream c2fbe40c */
    row = make_param_choice(row, "Decoder text", " Large \n Compact ", cfg_sm.p_digi_text_compact);
    /* FT8/FT4 table: distance, country or both, from SP2KMO's upstream 3074180e.
     * Four columns: "Country + km" ran over the arrow at three. */
    wl_row_label(row, "FT8/FT4 info");
    row_choice(row++, 1, 4, SMALL_4, " km \n mi \n Country \n Country + km \n Country + mi ", cfg_sm.p_ft8_info.get(),
               int_choice_cb, &cfg_sm.p_ft8_info);
    card_end(row);
}

static void make_audio_page() {
    x6100_base_ver_t base = x6100_control_get_base_ver();
    uint8_t          row;

    grid_create();
    row = card_begin("RX AUDIO");
    row = make_sp_mode(row);
    row = make_line_gain(row);
    row = make_audio_gain(row);
    row = make_param_switch(row, "Boot sound", cfg_sm.p_boot_sound);   /* CQ CQ in CW with the boot logo */
    card_end(row);

    row = card_begin("SCREEN REC");
    row = make_screen_rec_split(row);
    card_end(row);

    if (base.rev >= 3) {
        row = card_begin("TX AUDIO");
        row = make_comp(row);
        if (base.rev >= 9) {
            row = make_tx_filter(row);
        }
        if (base.rev >= 8) {
            row = make_param_switch(row, "FM pre/de-emphasis", cfg_sm.p_fm_emphasis);
        }
        card_end(row);
    }

    row = card_begin("MICROPHONE");
    row = make_mic(row, "Input", cfg_sm.p_mic);
    row = make_param_slider(row, "Hand mic", &hmic_slider);
    row = make_param_slider(row, "Built-in", &imic_slider);
    card_end(row);

    row = card_begin("VOX");
    row = make_param_switch(row, "On", cfg_sm.p_vox_en);
    row = make_param_slider(row, "Gain", &vox_gain_slider);
    row = make_param_slider(row, "Anti-VOX", &vox_ag_slider);
    row = make_param_slider(row, "Delay", &vox_delay_slider);
    card_end(row);

    row = card_begin("AGC");
    row = make_param_switch(row, "Hang", cfg_sm.p_agc_hang);
    row = make_param_slider(row, "Knee", &agc_knee_slider);
    row = make_param_slider(row, "Slope", &agc_slope_slider);
    card_end(row);
}

static void make_wireless_page() {
    grid_delete();
    grid_create();
    uint8_t row;

    wl_tab = "Wireless";
    row = card_begin("WIFI");
    row = make_wifi(row);
    card_end(row);

    eth_info_t in;

    eth_get_info(&in);
    eth = {};
    eth_shown = in.present;
    if (eth_shown) {
        row = card_begin("ETHERNET");
        row = make_ethernet(row);
        card_end(row);
    }

    lv_obj_add_event_cb(grid, wl_delete_cb, LV_EVENT_DELETE, NULL);
    wl_timer = lv_timer_create(wl_timer_cb, 1000, NULL);
    wifi_update();
    eth_update();
}

/* Bluetooth: the adapter, the devices (bt_dev.c), the headphone audio
 * (btaudio-ctl) and the CAT bridge (btcat). Nothing here waits: bt_dev's
 * worker talks to BlueZ, the page reads what it saw. */

static void bt_adapter_update() {
    bt_adapter_t a;
    int          left = bt_dev_visible_s();
    const char  *text = "";

    bt_dev_adapter(&a);
    if (!a.seen) {
        wl_pill(bt.pill, NULL, 0, 0);
        text = "-";
    } else if (!a.bluez) {
        wl_pill(bt.pill, "off", theme_hex(TC_chip), theme_hex(TC_text_value));
        text = "Bluetooth service not running";
    } else if (!a.present) {
        wl_pill(bt.pill, "off", theme_hex(TC_chip), theme_hex(TC_text_value));
        text = params.wifi_enabled.x ? "no adapter" : "WiFi/BT chip off (Wireless)";
    } else if (!a.powered) {
        wl_pill(bt.pill, "off", theme_hex(TC_chip), theme_hex(TC_text_value));
    } else if (left > 0) {
        wl_pill(bt.pill, "pairing", theme_hex(TC_pill_hp), theme_hex(TC_accent));
        text = "visible and pairable";
    } else {
        wl_pill(bt.pill, "on", theme_hex(TC_ok_bg), theme_hex(TC_ok));
        text = "hidden";
    }
    lv_label_set_text(bt.state, text);
    lv_label_set_text(bt.addr, a.present && a.addr[0] ? a.addr : "-");
}

/* btcat writes the number of its clients while it runs */
static void bt_cat_update() {
    int   n = -1;
    FILE *f = fopen("/tmp/btcat.clients", "r");

    if (f) {
        if (fscanf(f, "%d", &n) != 1) n = 0;
        fclose(f);
    }
    if (n < 0) {
        lv_label_set_text(bt.cat, "bridge not running");
    } else if (n == 0) {
        lv_label_set_text(bt.cat, "none connected");
    } else {
        lv_label_set_text_fmt(bt.cat, "%i connected", n);
    }
}

static void bt_timer_cb(lv_timer_t *t) {
    bt_update();
    bt_adapter_update();
    bt_cat_update();
    if (++bt_ticks % 2 == 0) {
        bt_dev_refresh();
    }
}

static void bt_page_delete_cb(lv_event_t *e) {
    wl_gen++;
    bt_forget_objs();
    lv_timer_del(bt_timer);
    bt_timer = NULL;
    wl_menu_back = NULL;
    wl_menu_close();
}

/* A value after a row label, as WiFi's Status: a pill and a text, or a text */
static lv_obj_t * bt_value_row(uint8_t row, const char *label, lv_obj_t **pill) {
    lv_obj_t *obj = lv_obj_create(grid);

    wl_row_label(row, label);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 12, 0);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_START, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    if (pill) *pill = wl_pill_create(obj, 0);
    return wl_label(obj, SF(22), theme_hex(TC_text), 0);
}

static void make_bluetooth_page() {
    grid_delete();
    grid_create();
    uint8_t row;

    wl_tab = "Bluetooth";
    bt.found.clear();
    bt.note.clear();
    bt.was_scanning = false;

    row = card_begin("ADAPTER");
    row = make_bt_power(row);
    bt.state = bt_value_row(row++, "Status", &bt.pill);
    bt.addr = bt_value_row(row++, "Address", NULL);
    row = make_bt_pairing(row);
    card_end(row);

    row = card_begin("DEVICES");
    row = make_bt_devices(row);
    card_end(row);

    row = card_begin("HEADPHONES");
    row = make_headphones(row);
    card_end(row);

    row = card_begin("BLUETOOTH CAT");
    bt.cat = bt_value_row(row++, "Clients", NULL);
    lv_label_set_text(bt_value_row(row++, "Port", NULL), "SPP, channel 1");
    card_end(row);

    lv_obj_add_event_cb(grid, bt_page_delete_cb, LV_EVENT_DELETE, NULL);
    bt_ticks = 0;
    bt_timer = lv_timer_create(bt_timer_cb, 1000, NULL);
    bt_dev_refresh();
    bt_update();
    bt_adapter_update();
    bt_cat_update();
    /* Headphone volume and RX switch once; wl_gen drops the answer if the page is left */
    btaudio_run("status", bt_status_done, (void *)(intptr_t)wl_gen);
}

static uint8_t info_row(uint8_t row, const char *label, const char *val) {
    wl_row_label(row, label);
    lv_obj_t *obj = lv_label_create(grid);
    lv_label_set_text(obj, val);
    lv_obj_set_style_text_color(obj, TC(text), 0);
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_END, 1, 6, LV_GRID_ALIGN_CENTER, row, 1);
    return row + 1;
}

/* A page object went with the page: the pointer to it is cleared */
static void ptr_clear_cb(lv_event_t *e) {
    *(lv_obj_t **)lv_event_get_user_data(e) = NULL;
}

/* Status line under an Update button: one line, cut with ... so the page fits without scrolling */
static lv_obj_t * upd_line(uint8_t row) {
    lv_obj_t *obj = lv_label_create(grid);

    lv_obj_set_style_text_color(obj, TC(text_dim), 0);
    lv_obj_set_style_text_font(obj, SF(16), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_obj_set_size(obj, lv_pct(100), lv_font_get_line_height(SF(16)));   /* fixed height: DOT cuts instead of wrapping */
    lv_obj_set_grid_cell(obj, LV_GRID_ALIGN_STRETCH, 0, 7, LV_GRID_ALIGN_CENTER, row, 1);
    row_dsc[row] = UPD_NOTE_H;
    return obj;
}

/* Broadcast list (Update): x6100-broadcast-update downloads EiBi's schedule for the SWL panel and
 * the broadcast name; a timer shows its progress file, at the end broadcast_db reads the new list. */
#define BC_CTL      "/usr/sbin/x6100-broadcast-update"
#define BC_INFO     "/mnt/broadcast.info"
#define BC_PROGRESS "/mnt/.broadcast.progress"

static lv_obj_t   *bc_note;
static lv_timer_t *bc_timer;
static bool        bc_busy;

static void bc_say(const char *s) {
    if (bc_note) lv_label_set_text(bc_note, s);
}

/* 9593 as "9,593" */
static void bc_thousands(char *out, size_t n, int v) {
    if (v >= 1000) snprintf(out, n, "%d,%03d", v / 1000, v % 1000);
    else snprintf(out, n, "%d", v);
}

static void bc_timer_cb(lv_timer_t *t) {
    FILE *f = fopen(BC_PROGRESS, "r");
    long got, total;
    char s[48];

    if (!f) return;
    if (fscanf(f, "%ld %ld", &got, &total) == 2) {
        if (total > 0) snprintf(s, sizeof(s), "Downloading %.1f of %.1f MB", got / 1e6, total / 1e6);
        else snprintf(s, sizeof(s), "Downloading %.1f MB", got / 1e6);
        bc_say(s);
    }
    fclose(f);
}

static void bc_note_del_cb(lv_event_t *e) {
    bc_note = NULL;
    if (bc_timer) {
        lv_timer_del(bc_timer);
        bc_timer = NULL;
    }
}

static void bc_done(const char *out, int status, void *user) {
    char kind[8] = "", reason[16] = "", season[8] = "", n[16], s[96];
    int stations = 0;

    bc_busy = false;
    if (bc_timer) {
        lv_timer_del(bc_timer);
        bc_timer = NULL;
    }
    if (sscanf(out, "%7s %d %7s", kind, &stations, season) == 3 && !strcmp(kind, "ok")) {
        broadcast_db_init();
        bc_thousands(n, sizeof(n), stations);
        snprintf(s, sizeof(s), "Broadcast list updated: %s stations, season %s.", n, season);
    } else {
        sscanf(out, "%7s %15s", kind, reason);
        if (!strcmp(reason, "offline")) strcpy(s, "No internet connection. Connect the radio to WiFi or Ethernet.");
        else if (!strcmp(reason, "busy")) strcpy(s, "An update of the broadcast list is already running.");
        else strcpy(s, "The download failed, the old list is still in use.");
    }
    if (bc_note) bc_say(s);
    else msg_update_text_fmt("%s", s);
}

static void bc_btn_cb(lv_event_t *e) {
    if (bc_busy) return;
    bc_busy = true;
    bc_say("Connecting to eibispace.de...");
    sh_run(BC_CTL, bc_done, NULL);
    if (!bc_timer) bc_timer = lv_timer_create(bc_timer_cb, 500, NULL);
}

static uint8_t make_broadcast_list(uint8_t row) {
    char s[96], n[16], season[8], day[12];
    int stations;
    struct stat st;
    FILE *f;

    wl_row_label(row, "Broadcast list");
    lv_obj_set_height(wl_button(row, "Update", bc_btn_cb), UPD_ROW_H - 2);
    row_dsc[row] = UPD_ROW_H;
    row++;
    bc_note = upd_line(row);
    lv_obj_add_event_cb(bc_note, bc_note_del_cb, LV_EVENT_DELETE, NULL);

    if (bc_busy) {
        bc_say("Downloading...");
        bc_timer = lv_timer_create(bc_timer_cb, 500, NULL);
        return row + 1;
    }
    f = fopen(BC_INFO, "r");
    if (f && fscanf(f, "%7s %11s %d", season, day, &stations) == 3) {
        bc_thousands(n, sizeof(n), stations);
        snprintf(s, sizeof(s), "In use: season %s, updated %s, %s stations.", season, day, n);
    } else if (stat("/mnt/broadcast.csv", &st) == 0) {
        strftime(day, sizeof(day), "%Y-%m-%d", localtime(&st.st_mtime));
        snprintf(s, sizeof(s), "In use: the list that came with the firmware, from %s.", day);
    } else {
        strcpy(s, "In use: the list that came with the firmware.");
    }
    if (f) fclose(f);
    bc_say(s);
    return row + 1;
}

/* Factory reset: a panel says what goes and what stays, nothing happens until the MFK is held
 * down 3 s (a press, a turn or ESC can't do it). Then the files go and the GUI quits: x6100_daemon
 * starts it again with the defaults. */
static lv_obj_t   *fr_btn;
static lv_obj_t   *fr_panel;
static lv_obj_t   *fr_count;
static lv_timer_t *fr_timer;
static uint32_t    fr_held;     /* ms the MFK has been down, 0: up */
static bool        fr_armed;    /* the MFK was seen up since the panel opened */

static void fr_close(void *arg) {
    if (fr_timer) {
        lv_timer_del(fr_timer);
        fr_timer = NULL;
    }
    if (!fr_panel) return;
    lv_obj_del(fr_panel);
    if (fr_btn) lv_group_focus_obj(fr_btn);
    lv_group_set_editing(keyboard_group, false);
}

static bool mfk_down(void) {
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_ENCODER) return i->proc.state == LV_INDEV_STATE_PRESSED;
    return false;
}

static void fr_timer_cb(lv_timer_t *t) {
    if (!mfk_down()) {
        if (fr_held) lv_label_set_text(fr_count, "Released: reset cancelled");
        fr_held  = 0;
        fr_armed = true;
        return;
    }
    if (!fr_armed) return;
    fr_held += 100;
    if (fr_held < 3000) {
        lv_label_set_text_fmt(fr_count, "Keep holding: %u s", (unsigned)((3000 - fr_held + 999) / 1000));
        return;
    }
    lv_label_set_text(fr_count, "Resetting...");
    lv_refr_now(NULL);
    factory_reset_dir("/mnt");
    _exit(0);   /* not exit(): no destructor may write the settings back */
}

static void fr_key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_ESC:    lv_async_call(fr_close, NULL); break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:   radio_change_vol(-1); break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:  radio_change_vol(1); break;
    }
}

static void fr_btn_cb(lv_event_t *e) {
    if (fr_panel) return;

    fr_panel = lv_obj_create(dialog.obj);
    lv_obj_add_event_cb(fr_panel, ptr_clear_cb, LV_EVENT_DELETE, &fr_panel);
    lv_obj_remove_style_all(fr_panel);
    lv_obj_set_size(fr_panel, lv_pct(94), lv_pct(92));
    lv_obj_center(fr_panel);
    lv_obj_add_style(fr_panel, &popup_style, 0);
    lv_obj_set_style_pad_all(fr_panel, 12, 0);
    lv_obj_set_style_pad_row(fr_panel, 8, 0);
    lv_obj_set_flex_flow(fr_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(fr_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *obj = wl_label(fr_panel, SF(22), theme_hex(TC_text_head), 0);
    lv_label_set_text(obj, "Factory reset");

    obj = wl_label(fr_panel, SF(18), theme_hex(TC_text_value), 0);
    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(obj, "Back to defaults: all settings with the band memory and the CW messages, app frequencies, "
                           "DX cluster, FT8 UDP, PSK squelch, TCI server, time zone, headphone volume, Bluetooth CAT.");
    obj = wl_label(fr_panel, SF(18), theme_hex(TC_text_dim), 0);
    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(obj, "Kept: logs (ADIF, QSO, field), memory channels, PSK/Olivia macros, JS8 texts, voice messages, "
                           "pictures, recordings, themes, CW trainer progress, Bluetooth pairings.");
    obj = wl_label(fr_panel, SF(18), theme_hex(TC_text_dim), 0);
    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(obj, "Then the screen goes dark for a few seconds while the radio software restarts.");

    lv_obj_set_flex_grow(wl_label(fr_panel, SF(16), theme_hex(TC_text_dim), 0), 1);   /* spacer */
    fr_count = wl_label(fr_panel, SF(22), theme_hex(TC_text_head), 0);
    lv_label_set_text(fr_count, "Hold MFK down 3 s to reset - ESC: cancel");

    fr_held  = 0;
    fr_armed = false;
    fr_timer = lv_timer_create(fr_timer_cb, 100, NULL);

    lv_group_add_obj(keyboard_group, fr_panel);
    lv_obj_add_event_cb(fr_panel, fr_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(fr_panel);
    lv_group_set_editing(keyboard_group, true);
}

static uint8_t make_factory_reset(uint8_t row) {
    wl_row_label(row, "Factory reset");
    fr_btn = wl_button(row, "Reset...", fr_btn_cb);
    lv_obj_add_event_cb(fr_btn, ptr_clear_cb, LV_EVENT_DELETE, &fr_btn);
    return row + 1;
}

/* Info: cards and rows as the other tabs, in the same font */
static void make_info_page() {
    grid_create();
    lv_obj_set_style_pad_bottom(page, 6, 0);   /* short page: it fits, no scrolling */
    uint8_t row;

    row = card_begin("FIRMWARE");
    row = info_row(row, "Version", strrchr(VERSION, ' ') + 1);
    row = info_row(row, "Project", "OpenX6X00 by RF.Guru");
    row = info_row(row, "Source", "github.com/Guru-RF/OpenX6X00");
    row = info_row(row, "Baseband", x6100_control_get_fw_version_str());
    row = make_broadcast_list(row);
    row = make_factory_reset(row);
    card_end(row);

    /* The voltages the top right box leaves out for the time left */
    float   ext, bat;
    uint8_t cap;
    bool    chg;
    int     hours, charges;
    char    s[64];

    clock_power_get(&ext, &bat, &cap, &chg);
    bat_est_learned(&hours, &charges);
    row = card_begin("POWER");
    if (bat >= 3.0f) {
        snprintf(s, sizeof(s), "%.1f V  %i%%", bat, cap);
    } else {
        snprintf(s, sizeof(s), "not fitted");
    }
    row = info_row(row, "Battery", s);
    if (ext >= 3.0f) {
        snprintf(s, sizeof(s), chg ? "%.1f V  charging" : "%.1f V", ext);
    } else {
        snprintf(s, sizeof(s), "none");
    }
    row = info_row(row, "External", s);
    if (hours || charges) {
        snprintf(s, sizeof(s), "learned from %i h on battery, %i charges", hours, charges);
    } else {
        snprintf(s, sizeof(s), "starting values, learning");
    }
    row = info_row(row, "Time left", s);
    card_end(row);
}

/* Update: version, the new-release alert, the app update */
static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    for (int i = 0; i < 3; i++) {
        btn_pages[i]->items[0]->next = btn_pages[(i + 1) % 3];
    }
    rail_create();
    tab_press(&btn_general);
    lv_group_set_focus_cb(keyboard_group, focus_cb);
}

static void destruct_cb() {
    lv_group_set_focus_cb(keyboard_group, NULL);
    grid_delete();
}

/* FINP on the hand mic: in and out of editing, as a long MFK press. ESC closes. */
static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case HKEY_FINP:
            lv_group_set_editing(keyboard_group, !lv_group_get_editing(keyboard_group));
            break;

        case LV_KEY_ESC:
            dialog_destruct();
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:   radio_change_vol(-1); break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:  radio_change_vol(1); break;
    }
}

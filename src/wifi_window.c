/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* The WiFi window: on/off, scan, the networks in reach (known ones first, signal bars),
 * connect, password, forget; the state, address and gateway on the left */

#include "wifi_window.h"
#include "button_bar.h"
#include "gui_events.h"
#include "key_input.h"
#include "msg_box.h"
#include "params/params.h"
#include "pubsub_ids.h"
#include "radio.h"
#include "gui_styles.h"
#include "text_popup.h"
#include "theme.h"
#include "wifi.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdlib.h>

#define WIN_W       775
#define WIN_H       (DIALOG_H - 26)     /* 13 px above and below */
#define INFO_W      300
#define BARS        5
#define BAR_W       10
#define BAR_GAP     5

typedef enum { PICK_NONE, PICK_NEW, PICK_KNOWN } pick_t;

static lv_obj_t      *nets, *state_val, *ip_val, *gw_val;
static lv_timer_t    *list_timer, *addr_timer;
static void          *state_sub;
static bool           typing;           /* the password keyboard is open: the buttons wait */
static pick_t         picked = PICK_NONE;
static wifi_ap_info_t editing;          /* the network the password is for */
static char          *typed;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void power_press(button_data_t *b);
static void scan_press(button_data_t *b);
static void connect_press(button_data_t *b);
static void password_press(button_data_t *b);
static void forget_press(button_data_t *b);
static const char *power_label(void);
static const char *scan_label(void);
static const char *connect_label(void);
static const char *password_label(void);
static const char *forget_label(void);

static button_data_t btn_power    = {.type = BTN_TEXT_FN, .label_fn = power_label, .press = power_press};
static button_data_t btn_scan     = {.type = BTN_TEXT_FN, .label_fn = scan_label, .press = scan_press};
static button_data_t btn_connect  = {.type = BTN_TEXT_FN, .label_fn = connect_label, .press = connect_press};
static button_data_t btn_password = {.type = BTN_TEXT_FN, .label_fn = password_label, .press = password_press};
static button_data_t btn_forget   = {.type = BTN_TEXT_FN, .label_fn = forget_label, .press = forget_press};

static buttons_page_t page = {{&btn_power, &btn_scan, &btn_connect, &btn_password, &btn_forget}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
    .audio_cb     = NULL,
    .key_cb       = key_cb,
};

dialog_t *dialog_wifi = &dialog;

/* ---- the list of networks ---- */

/* The network on the selected row, NULL for none (or an empty row) */
static wifi_ap_info_t *selected(bool *any_row) {
    uint16_t row, col;

    lv_table_get_selected_cell(nets, &row, &col);
    *any_row = row != LV_TABLE_CELL_NONE && col != LV_TABLE_CELL_NONE;
    return *any_row ? lv_table_get_cell_user_data(nets, row, col) : NULL;
}

static void state_changed(void) {
    lv_msg_send(MSG_WIFI_STATE_CHANGED, NULL);
}

/* Connected first, then the known ones */
static int by_rank(const void *a, const void *b) {
    const wifi_ap_info_t *x = a, *y = b;
    int                   rx = x->is_connected * 2 + x->known, ry = y->is_connected * 2 + y->known;

    return rx == ry ? 0 : rx < ry ? 1 : -1;
}

static void list_empty(void) {
    lv_table_set_cell_value(nets, 0, 0, "");
    lv_table_set_cell_user_data(nets, 0, 0, NULL);
    lv_table_set_row_cnt(nets, 1);
}

static void list_refresh_cb(lv_timer_t *t) {
    wifi_ap_arr_t found;

    if (!params.wifi_enabled.x) {
        lv_timer_del(list_timer);
        list_timer = NULL;
    }
    found = wifi_get_available_access_points();
    if (found.count) {
        qsort(found.ap_arr, found.count, sizeof(wifi_ap_info_t), by_rank);
    }
    for (uint16_t i = 0; i < found.count; i++) {
        wifi_ap_info_t *ap = malloc(sizeof(wifi_ap_info_t));

        *ap = found.ap_arr[i];
        lv_table_set_cell_value_fmt(nets, i, 0, "%s %s", ap->ssid, ap->is_connected ? " (*)" : "");
        lv_table_set_cell_user_data(nets, i, 0, ap);
    }
    if (found.count) {
        lv_table_set_row_cnt(nets, found.count);
    } else {
        list_empty();
    }
    lv_event_send(nets, LV_EVENT_VALUE_CHANGED, NULL);
    wifi_aps_info_delete(found);
}

static void list_start(void) {
    list_timer = lv_timer_create(list_refresh_cb, 1000, NULL);
}

static void list_stop(void) {
    if (list_timer) {
        lv_timer_del(list_timer);
        list_timer = NULL;
    }
}

/* A row chosen: the buttons follow what kind of network it is */
static void pick_cb(lv_event_t *e) {
    bool            any;
    wifi_ap_info_t *ap = selected(&any);
    pick_t          now = !ap ? PICK_NONE : ap->known ? PICK_KNOWN : PICK_NEW;

    if (!any || now != picked) {
        picked = now;
        state_changed();
    }
}

/* Signal bars at the right end of a row */
static void row_draw_cb(lv_event_t *e) {
    lv_obj_t               *table = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) {
        return;
    }
    uint32_t        cols = lv_table_get_col_cnt(table);
    wifi_ap_info_t *ap = lv_table_get_cell_user_data(table, dsc->id / cols, dsc->id % cols);

    if (!ap) {
        return;
    }
    lv_draw_rect_dsc_t bar;
    uint8_t            lit = ap->strength * BARS / 100;
    lv_area_t          a = {.x1 = dsc->draw_area->x2 - BARS * (BAR_W + BAR_GAP), .y2 = dsc->draw_area->y2 - 5};

    lv_draw_rect_dsc_init(&bar);
    bar.bg_color     = TC(text);
    bar.bg_opa       = LV_OPA_COVER;
    bar.border_width = 1;
    bar.border_color = TC(text);
    for (int i = 0; i < BARS; i++) {
        a.x2 = a.x1 + BAR_W;
        a.y1 = a.y2 - (i + 1) * 5;
        if (i > lit) {
            bar.bg_opa = LV_OPA_TRANSP;     /* outlined only */
        }
        lv_draw_rect(dsc->draw_ctx, &bar, &a);
        a.x1 += BAR_W + BAR_GAP;
    }
}

/* ---- the password keyboard ---- */

static void typing_done(void) {
    textarea_window_close();
    lv_group_add_obj(keyboard_group, nets);
    lv_group_set_editing(keyboard_group, true);
    typing = false;
}

static bool password_cancel(void) {
    msg_update_text_fmt("Password is required");
    typing_done();
    return true;
}

static bool password_ok(void) {
    typed = (char *)textarea_window_get();
    if (editing.password_validator && !editing.password_validator(typed)) {
        msg_update_text_fmt("Incorrect password");
        return false;
    }
    if (editing.known) {
        wifi_update_connection(editing.ssid, typed);
    } else {
        wifi_add_connection(editing.ssid, typed);
    }
    typed = NULL;
    typing_done();
    return true;
}

static void ask_password(const wifi_ap_info_t *ap) {
    editing = *ap;
    lv_group_remove_obj(nets);
    textarea_window_open(password_ok, password_cancel);
    if (typed) {
        textarea_window_set(typed);
    } else {
        lv_textarea_set_placeholder_text(textarea_window_text(), " Password");
    }
    typing = true;
}

/* ---- buttons ---- */

static bool radio_up(void) {
    wifi_status_t s = wifi_get_status();

    return s != WIFI_OFF && s != WIFI_STARTING;
}

static void power_press(button_data_t *b) {
    if (typing) {
        return;
    }
    if (params.wifi_enabled.x) {
        list_stop();
        wifi_power_off();
        list_empty();
        lv_event_send(nets, LV_EVENT_VALUE_CHANGED, NULL);
    } else {
        wifi_power_on();
        list_start();
        msg_update_text_fmt("Turning on");
    }
}

static void scan_press(button_data_t *b) {
    if (typing || !radio_up()) {
        return;
    }
    if (wifi_scanning()) {
        msg_update_text_fmt("Already scanning");
    } else {
        msg_update_text_fmt("Start scan");
        wifi_start_scan();
    }
}

static void connect_press(button_data_t *b) {
    bool            any;
    wifi_ap_info_t *ap;

    if (typing) {
        return;
    }
    switch (wifi_get_status()) {
        case WIFI_DISCONNECTED:
            ap = selected(&any);
            if (!ap) {
                LV_LOG_USER("Nothing selected, can't connect");
            } else if (ap->wpa3_only) {
                msg_update_text_fmt("WPA3-only network: set the router to WPA2 or WPA2/WPA3");
            } else if (ap->known) {
                wifi_connect(ap->ssid);
            } else if (!ap->password_validator) {
                wifi_add_connection(ap->ssid, NULL);    /* an open network */
            } else {
                ask_password(ap);
            }
            break;

        case WIFI_CONNECTED:
            wifi_disconnect();
            break;

        default:
            break;
    }
}

static void password_press(button_data_t *b) {
    bool            any;
    wifi_ap_info_t *ap = selected(&any);

    if (!ap) {
        LV_LOG_USER("Nothing selected");
    } else if (!ap->known) {
        msg_update_text_fmt("Can't update new connection");
    } else if (!ap->password_validator) {
        msg_update_text_fmt("Password is not used");
    } else {
        ask_password(ap);
    }
}

static void forget_press(button_data_t *b) {
    bool            any;
    wifi_ap_info_t *ap = selected(&any);

    if (!ap) {
        LV_LOG_USER("Nothing selected");
        return;
    }
    wifi_delete_connection(ap->ssid);
    msg_update_text_fmt("Connection deleted");
}

static const char *power_label(void) {
    switch (wifi_get_status()) {
        case WIFI_OFF:      return "Wifi/BT:\nOff";
        case WIFI_STARTING: return "Wifi/BT:\nStarting...";
        default:            return "Wifi/BT:\nOn";
    }
}

static const char *scan_label(void) {
    return !radio_up() ? "" : wifi_scanning() ? "Scanning..." : "Scan";
}

static const char *connect_label(void) {
    switch (wifi_get_status()) {
        case WIFI_DISCONNECTED: return picked != PICK_NONE ? "Connect" : "";
        case WIFI_CONNECTED:    return "Disconnect";
        case WIFI_CONNECTING:   return "Connecting...";
        default:                return "";
    }
}

/* Only for a saved network, when not busy connecting */
static bool known_idle(void) {
    wifi_status_t s = wifi_get_status();

    return picked == PICK_KNOWN && (s == WIFI_DISCONNECTED || s == WIFI_CONNECTED);
}

static const char *password_label(void) {
    return known_idle() ? "Change\npassword" : "";
}

static const char *forget_label(void) {
    return known_idle() ? "Delete" : "";
}

/* ---- state, address ---- */

/* Until the address agrees with the state (NetworkManager sets it a moment later) */
static void address_cb(lv_timer_t *t) {
    char  ip[16], gw[16];
    char *ip_p = ip, *gw_p = gw;
    bool  got = wifi_get_ipaddr(&ip_p, &gw_p);

    if (got != (wifi_get_status() == WIFI_CONNECTED)) {
        return;
    }
    lv_label_set_text(ip_val, got ? ip : "N/A");
    lv_label_set_text(gw_val, got ? gw : "N/A");
    if (addr_timer) {
        lv_timer_del(addr_timer);
        addr_timer = NULL;
    }
}

static void state_cb(void *s, lv_msg_t *m) {
    buttons_page_t *cur = buttons_get_cur_page();
    const char     *text;

    if (cur) {
        for (int i = 0; i < BUTTONS; i++) {
            buttons_refresh(cur->items[i]);
        }
    }
    switch (wifi_get_status()) {
        case WIFI_CONNECTED:  text = "Connected"; break;
        case WIFI_CONNECTING: text = "Connecting..."; break;
        case WIFI_STARTING:   text = "Starting..."; break;
        default:              text = "Disconnected"; break;
    }
    lv_label_set_text(state_val, text);
    if (!addr_timer) {
        addr_timer = lv_timer_create(address_cb, 100, NULL);
    }
}

/* ---- the window ---- */

static lv_style_t value_gap;       /* a value indented, a gap under it */

static lv_obj_t *info_value(lv_obj_t *col, const char *title, const char *value) {
    lv_obj_t *l = lv_label_create(col);

    lv_label_set_text(l, title);
    l = lv_label_create(col);
    lv_label_set_text(l, value);
    lv_obj_add_style(l, &value_gap, 0);
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    lv_obj_t *row, *col;

    dialog.obj = dialog_init(parent);
    lv_style_init(&value_gap);
    lv_style_set_pad_bottom(&value_gap, 20);
    lv_style_set_pad_left(&value_gap, 10);
    if (params.wifi_enabled.x) {
        list_start();
    }

    row = lv_obj_create(dialog.obj);
    lv_obj_remove_style(row, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_size(row, WIN_W, WIN_H);
    lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    col = lv_obj_create(row);
    lv_obj_remove_style(col, NULL, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_size(col, INFO_W, WIN_H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    state_val = info_value(col, "Status:", "Unknown");
    ip_val    = info_value(col, "IP address:", "N/A");
    gw_val    = info_value(col, "Gateway:", "N/A");

    nets = lv_table_create(row);
    lv_table_set_col_cnt(nets, 1);
    lv_table_set_col_width(nets, 0, WIN_W - INFO_W - 2);
    lv_obj_set_height(nets, WIN_H);
    lv_obj_center(nets);
    lv_obj_set_flex_grow(nets, 1);
    lv_obj_remove_style(nets, NULL, LV_PART_MAIN | LV_PART_ITEMS | LV_STATE_ANY);
    lv_obj_set_style_bg_opa(nets, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(nets, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_border_width(nets, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(nets, TC(dialog_border), LV_PART_MAIN);
    lv_obj_set_style_border_opa(nets, 128, LV_PART_MAIN);
    lv_obj_set_style_border_width(nets, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(nets, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(nets, TC(focus), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(nets, LV_OPA_30, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_pad_top(nets, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(nets, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(nets, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(nets, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(nets, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(nets, pick_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(nets, row_draw_cb, LV_EVENT_DRAW_PART_END, NULL);
    lv_group_add_obj(keyboard_group, nets);
    lv_group_set_editing(keyboard_group, true);

    state_sub = lv_msg_subscribe(MSG_WIFI_STATE_CHANGED, state_cb, NULL);
    state_changed();
}

static void destruct_cb(void) {
    if (state_sub) {
        lv_msg_unsubscribe(state_sub);
        state_sub = NULL;
    }
    if (addr_timer) {
        lv_timer_del(addr_timer);
        addr_timer = NULL;
    }
    typing_done();
    list_stop();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC) {
        dialog_destruct();
    } else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT) {
        radio_change_vol(-1);
    } else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT) {
        radio_change_vol(1);
    }
}

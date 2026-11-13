/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the first-start guide: three steps over the main screen, WiFi, the callsign and locator, the clock and
 *  its time zone, each with Back, Skip and Next, then a last screen with what was set and where the manual and the web
 *  configurator are. An app window like the others: F1-F5 for its buttons, the MFK walks its controls and a press
 *  chooses, a touch does the same. The radio receives under it all the while, the main knob tunes and VOL is the
 *  volume. It reuses what Settings has: the WiFi window for the network, the station's checked keyboard entry for the
 *  callsign and locator, the clock's time zones and date and time fields.
 *
 *  It comes up by itself once (first_start_rule.h): a new card, or the settings reset. Settings > General > STATION >
 *  First-start guide runs it again.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "first_start.h"

#include "first_start_rule.h"
#include "button_bar.h"
#include "cfg/cfg_api.h"
#include "clock.h"
#include "ft8/utils.h"
#include "gps.h"
#include "gui_events.h"
#include "gui_styles.h"
#include "key_input.h"
#include "msg_box.h"
#include "params/db.h"
#include "params/params.h"
#include "qth/qth.h"
#include "radio.h"
#include "screen_main.h"
#include "settings_window.h"
#include "station.h"
#include "text_popup.h"
#include "theme.h"
#include "walkie.h"
#include "wifi.h"
#include "wifi_window.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define START_MS    3000    /* after the start: the screen up, the radio receiving, the settings note said */
#define PAD         16
#define LABEL_W     190     /* a row's name */
#define ROW_H       44
#define ITEMS       10
#define GPS_AGE_S   10      /* a fix this recent is where the radio is, as station_qth() takes it */
#define TIME_SYNC   "/var/run/time-sync"    /* ntpdate set the clock when the network came up (99-ntp-sync.sh) */

enum { STEP_WIFI, STEP_STATION, STEP_TIME, STEP_DONE, STEPS };
enum { KBD_NONE, KBD_CALLSIGN, KBD_QTH };

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void back_press(button_data_t *b);
static void act1_press(button_data_t *b);
static void act2_press(button_data_t *b);
static void skip_press(button_data_t *b);
static void next_press(button_data_t *b);
static const char *back_label(void);
static const char *act1_label(void);
static const char *act2_label(void);
static const char *skip_label(void);
static const char *next_label(void);

static button_data_t btn_back = {.type = BTN_TEXT_FN, .label_fn = back_label, .press = back_press};
static button_data_t btn_act1 = {.type = BTN_TEXT_FN, .label_fn = act1_label, .press = act1_press};
static button_data_t btn_act2 = {.type = BTN_TEXT_FN, .label_fn = act2_label, .press = act2_press};
static button_data_t btn_skip = {.type = BTN_TEXT_FN, .label_fn = skip_label, .press = skip_press};
static button_data_t btn_next = {.type = BTN_TEXT_FN, .label_fn = next_label, .press = next_press};

static buttons_page_t page = {{&btn_back, &btn_act1, &btn_act2, &btn_skip, &btn_next}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
};

dialog_t *dialog_first_start = &dialog;

static bool              shown;         /* the window is up (dialog.obj stays set after a close) */
static int               step;
static bool              skipped[STEPS];
static bool              detour;        /* gave way to the WiFi window: back when it closes, nothing marked */
static bool              finished;      /* Done: closed with nothing more to say */
static first_start_esc_t esc;
static int               typing;        /* the keyboard open for KBD_CALLSIGN or KBD_QTH */
static int               focus_back;    /* the control to focus once the step is built again, its place in items */

static lv_obj_t   *body;                /* the step's content, built again for each step */
static lv_obj_t   *items[ITEMS];        /* its controls, in the order the MFK walks them */
static int         items_n;
static lv_obj_t   *status;              /* the WiFi or clock line */
static lv_obj_t   *clock_now, *clock_date;
static lv_obj_t   *zone_dd;
static lv_obj_t   *dt_spin[6];
static lv_timer_t *tick;

/* What a step was built with: looked at again as the clock ticks, and the step built again when one changed */
static char        gps_grid[7];         /* the locator of a current fix, "" none: step 2 offers it */
static const char *kept_shown;          /* step 3: what keeps the clock */

static struct tm dt_tm;                 /* step 3: the date and time fields */
static bool      dt_changed;            /* the clock set by them, not yet in the clock chip */
static bool      dt_refresh;            /* the fields being set from the clock: not an edit */

/* ---- what the radio has ---- */

/* The locator of a current fix, 6 characters, into grid; "" none */
static void gps_locator(char *grid) {
    double lat, lon;
    int    age;
    char   full[9];

    grid[0] = 0;
    if (gps_last_fix(&lat, &lon, &age) && age < GPS_AGE_S) {
        qth_pos_to_str(lat, lon, full);
        snprintf(grid, 7, "%s", full);
    }
}

/* What keeps the clock right: the GPS (gpsd feeds ntpd, as FT8's Time Sync takes it), NTP (ntpd, or ntpdate when the
 * network came up), NULL nothing */
static const char *clock_kept_by(void) {
    double lat, lon;
    int    age;

    if (gps_last_fix(&lat, &lon, &age) && age <= GPS_AGE_S) {
        return "the GPS";
    }
    if (ftx_clock_synced(NULL) || access(TIME_SYNC, F_OK) == 0) {
        return "NTP";
    }
    return NULL;
}

/* The network joined, into name; false none */
static bool wifi_network(char *name, size_t size) {
    wifi_ap_arr_t found = wifi_get_available_access_points();
    bool          got   = false;

    for (int i = 0; i < found.count && !got; i++) {
        if (found.ap_arr[i].is_connected) {
            snprintf(name, size, "%s", found.ap_arr[i].ssid);
            got = true;
        }
    }
    wifi_aps_info_delete(found);
    return got;
}

/* The radio's address on the network, into ip; false none */
static bool net_address(char *ip, size_t size) {
    char  a[16] = "", g[16] = "";
    char *ap = a, *gp = g;

    if (wifi_eth_active()) {
        eth_info_t eth;

        eth_get_info(&eth);
        snprintf(ip, size, "%s", eth.addr);
        ip[strcspn(ip, "/")] = 0;   /* 192.168.1.20/24 */
        return ip[0];
    }
    if (wifi_get_status() == WIFI_CONNECTED && wifi_get_ipaddr(&ap, &gp) && a[0]) {
        snprintf(ip, size, "%s", a);
        return true;
    }
    return false;
}

/* Step 1's line: as the WiFi window says it, with the network and the address once joined */
static bool wifi_line(char *buf, size_t size) {
    char name[33], ip[20];

    if (wifi_eth_active()) {
        net_address(ip, sizeof(ip));
        snprintf(buf, size, "Ethernet connected, %s: WiFi is not needed", ip);
        return true;
    }
    switch (wifi_get_status()) {
        case WIFI_OFF:
            snprintf(buf, size, "WiFi is off: Networks turns it on");
            return false;
        case WIFI_STARTING:
            snprintf(buf, size, "WiFi starting...");
            return false;
        case WIFI_CONNECTING:
            snprintf(buf, size, "Connecting...");
            return false;
        case WIFI_CONNECTED:
            if (!wifi_network(name, sizeof(name))) {
                snprintf(name, sizeof(name), "the network");
            }
            if (net_address(ip, sizeof(ip))) {
                snprintf(buf, size, "Connected to %s, %s", name, ip);
            } else {
                snprintf(buf, size, "Connected to %s", name);
            }
            return true;
        default:
            snprintf(buf, size, "Not connected: Networks lists the networks in reach");
            return false;
    }
}

/* ---- the step's controls ---- */

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *s) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, s);
    return l;
}

/* A line across the window, wrapped */
static lv_obj_t *para(const lv_font_t *font, lv_color_t color, const char *s) {
    lv_obj_t *l = text(body, font, color, s);

    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

/* A row: its name at the left (none: NULL), what comes after it in a row */
static lv_obj_t *row(const char *name) {
    lv_obj_t *r = lv_obj_create(body);

    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, ROW_H, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (name) {
        lv_obj_set_width(text(r, &sony_22, TC(text_dim), name), LABEL_W);
    }
    return r;
}

/* A control: the theme's focus look, in the keyboard group, the guide's keys */
static void item(lv_obj_t *obj) {
    dialog_item(&dialog, obj);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, TC(control_border), 0);
    if (items_n < ITEMS) {
        items[items_n++] = obj;
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *label, lv_coord_t w, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(parent);

    lv_obj_remove_style_all(b);
    item(b);
    lv_obj_set_size(b, w, ROW_H - 4);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(text(b, &sony_22, TC(text), label));
    return b;
}

/* ---- step 1: WiFi ---- */

/* The WiFi window closed: the guide again on its step, unless an app was opened in its place (APP), which stays; the
 * guide is then left */
static void wifi_back(void *arg) {
    if (dialog_is_run()) {
        detour = false;
        param_i_set(cfg_first_start_done, 1);
        return;
    }
    main_screen_start_app(ACTION_APP_FIRST_START);
}

/* WiFi on first: the window lists the networks only while it is on */
static void wifi_open(void *arg) {
    if (!shown) {
        return;
    }
    if (!params.wifi_enabled.x) {
        wifi_power_on();
    }
    detour = true;
    dialog_on_close(dialog_wifi, wifi_back);
    main_screen_start_app(ACTION_APP_WIFI);
}

/* After the press, not in it: the window goes with the object pressed */
static void networks_cb(lv_event_t *e) {
    lv_async_call(wifi_open, NULL);
}

static void build_wifi(void) {
    char line[96];
    bool ok = wifi_line(line, sizeof(line));

    para(&sony_22, TC(text), "WiFi sets the clock, and the DX Cluster, the uploads and the web configurator need it. "
                             "Networks opens the WiFi window: choose a network there, then ESC or GEN comes back here.");
    status = para(&sony_24, ok ? TC(ok) : TC(text_value), line);
    button(row(NULL), "Networks", 220, networks_cb);
}

/* ---- step 2: callsign and locator ---- */

static void build(void);

/* The keyboard gets all the keys: the step is built again when it closes */
static bool kbd_close(void) {
    focus_back = typing == KBD_QTH ? 1 : 0;
    typing     = KBD_NONE;
    textarea_window_close();
    build();
    return true;
}

static bool kbd_ok(void) {
    const char *val = textarea_window_get();
    bool        ok  = typing == KBD_CALLSIGN ? station_callsign_save(val) : station_qth_save(val);

    return ok && kbd_close();
}

/* The station's own entry (station.c): the characters a callsign or a locator takes, checked on OK as Settings does.
 * The locator starts from a current fix when none is saved */
static void kbd_open(int which) {
    if (typing || !shown) {
        return;
    }
    typing = which;
    lv_group_remove_all_objs(keyboard_group);
    textarea_window_open(kbd_ok, kbd_close);
    if (which == KBD_CALLSIGN) {
        station_callsign_edit(textarea_window_text());
    } else {
        station_qth_edit(textarea_window_text());
        gps_locator(gps_grid);
        if (!params.qth.x[0] && gps_grid[0]) {
            textarea_window_set(gps_grid);
        }
    }
}

static void callsign_cb(lv_event_t *e) {
    kbd_open(KBD_CALLSIGN);
}

static void qth_cb(lv_event_t *e) {
    kbd_open(KBD_QTH);
}

static void gps_use_cb(lv_event_t *e) {
    gps_locator(gps_grid);
    if (gps_grid[0] && station_qth_save(gps_grid)) {
        msg_update_text_fmt("Locator %s from the GPS", gps_grid);
        focus_back = 1;
        build();
    }
}

static void build_station(void) {
    char use[24];

    para(&sony_22, TC(text), "FT8, JS8, WSPR, the CW messages and the DX Cluster need your callsign. The locator is "
                             "where you are: the maps, the distances and the band plan go by it.");
    button(row("Callsign"), params.callsign.x[0] ? params.callsign.x : "-", 220, callsign_cb);
    button(row("Locator"), params.qth.x[0] ? params.qth.x : "-", 220, qth_cb);
    gps_locator(gps_grid);
    if (gps_grid[0]) {
        snprintf(use, sizeof(use), "Use %s", gps_grid);
        button(row("GPS fix"), use, 220, gps_use_cb);
    } else {
        para(&sony_20, TC(text_dim), "With a GPS that has a fix, its locator is offered here.");
    }
}

/* ---- step 3: the clock and its time zone ---- */

static void clock_show(void) {
    time_t      now = time(NULL);
    struct tm   loc, utc;
    const char *ampm;
    char        date[32];
    int         h;

    localtime_r(&now, &loc);
    gmtime_r(&now, &utc);
    h = clock_hour(loc.tm_hour, &ampm);
    lv_label_set_text_fmt(clock_now, ampm[0] ? "%i:%02i:%02i%s" : "%02i:%02i:%02i%s", h, loc.tm_min, loc.tm_sec, ampm);
    strftime(date, sizeof(date), "%a %d %b %Y", &loc);
    lv_label_set_text_fmt(clock_date, "%s, %02i:%02i UTC", date, utc.tm_hour, utc.tm_min);
}

/* The fields from the clock, while none is being changed */
static void dt_show(void) {
    time_t now = time(NULL);

    for (int i = 0; i < 6; i++) {
        if (!dt_spin[i] || lv_obj_has_state(dt_spin[i], LV_STATE_EDITED)) {
            return;
        }
    }
    localtime_r(&now, &dt_tm);
    dt_refresh = true;
    for (int i = 0; i < 6; i++) {
        lv_spinbox_set_value(dt_spin[i], *clock_field(&dt_tm, i) + clock_fields[i].base);
    }
    dt_refresh = false;
}

/* As Settings: only what the edit changed, on the clock as it is now (clock_fields_set) */
static void dt_changed_cb(lv_event_t *e) {
    struct tm was = dt_tm;

    if (dt_refresh) {
        return;
    }
    for (int i = 0; i < 6; i++) {
        *clock_field(&dt_tm, i) = lv_spinbox_get_value(dt_spin[i]) - clock_fields[i].base;
    }
    if (clock_fields_set(&was, &dt_tm)) {
        dt_changed = true;
        clock_show();
    }
}

/* Into the clock chip once the edit is over: the focus back on a field, or the step left */
static void dt_save(void) {
    if (dt_changed && clock_rtc_save()) {
        dt_changed = false;
    }
}

static void dt_rtc_cb(lv_event_t *e) {
    if (!lv_obj_has_state(lv_event_get_target(e), LV_STATE_EDITED)) {
        dt_save();
    }
}

static void zone_cb(lv_event_t *e) {
    clock_tz_set(clock_zones[lv_dropdown_get_selected(lv_event_get_target(e))].posix);
    clock_show();
    dt_show();
}

/* ESC on the open list only closes it (the dropdown does that before the guide's key_cb sees the key) */
static bool list_was_open;

static void zone_key_cb(lv_event_t *e) {
    list_was_open = lv_dropdown_is_open(lv_event_get_target(e));
}

static void dt_fields(lv_obj_t *r, int first) {
    static const lv_coord_t w[6] = {56, 56, 84, 56, 56, 56};

    for (int i = first; i < first + 3; i++) {
        const clock_field_t *f  = &clock_fields[i];
        lv_obj_t            *sb = lv_spinbox_create(r);

        item(sb);
        lv_obj_set_style_text_font(sb, &sony_24, 0);
        lv_spinbox_set_range(sb, f->lo, f->hi);
        lv_spinbox_set_digit_format(sb, f->digits, 0);
        lv_spinbox_set_digit_step_direction(sb, LV_DIR_LEFT);
        lv_spinbox_set_value(sb, *clock_field(&dt_tm, i) + f->base);
        lv_obj_set_style_pad_top(sb, 7, 0);       /* the digits in the middle of the box */
        lv_obj_set_style_pad_bottom(sb, 1, 0);
        lv_obj_set_style_pad_hor(sb, 6, 0);
        lv_obj_set_size(sb, w[i], ROW_H - 4);
        lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(sb, dt_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(sb, dt_rtc_cb, LV_EVENT_FOCUSED, NULL);
        dt_spin[i] = sb;
    }
}

static void build_time(void) {
    static char options[1024];
    char        line[96];
    lv_obj_t   *r;
    size_t      n = 0;
    time_t      now = time(NULL);

    kept_shown = clock_kept_by();
    r = row(NULL);
    clock_now  = text(r, &sony_36, TC(text_value), "");
    clock_date = text(r, &sony_22, TC(text_dim), "");
    clock_show();
    if (kept_shown) {
        snprintf(line, sizeof(line), "Set by %s: right to the second, nothing to do here", kept_shown);
    } else {
        snprintf(line, sizeof(line), "Not set by NTP or a GPS: set it below, or join WiFi (step 1)");
    }
    status = para(&sony_22, kept_shown ? TC(ok) : TC(warn), line);

    for (size_t i = 0; i < clock_zones_n && n < sizeof(options) - 64; i++) {
        n += snprintf(options + n, sizeof(options) - n, "%s%s", i ? "\n" : "", clock_zones[i].label);
    }
    zone_dd = lv_dropdown_create(row("Time zone"));
    item(zone_dd);
    styles_dropdown_list(lv_dropdown_get_list(zone_dd));
    lv_dropdown_set_options(zone_dd, options);
    lv_dropdown_set_symbol(zone_dd, NULL);
    lv_dropdown_set_selected(zone_dd, clock_zone_now());
    lv_obj_set_style_text_font(zone_dd, &sony_22, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(zone_dd), &sony_22, 0);
    lv_obj_set_style_pad_ver(zone_dd, 8, 0);
    lv_obj_set_size(zone_dd, 400, ROW_H - 4);
    lv_obj_add_event_cb(zone_dd, zone_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(zone_dd, zone_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);

    /* By hand only when nothing keeps it: set by NTP or the GPS, a field moved would only put it wrong */
    memset(dt_spin, 0, sizeof(dt_spin));
    if (!kept_shown) {
        localtime_r(&now, &dt_tm);
        r = row("Date, time");
        dt_fields(r, 0);
        lv_obj_set_style_pad_left(text(r, &sony_22, TC(text_dim), ""), 12, 0);
        dt_fields(r, 3);
    } else {
        para(&sony_20, TC(text_dim), "FT8, JS8 and WSPR need the clock right to a second or two. The logs and the "
                                     "digital modes keep UTC; the screen's clock shows this time zone.");
    }
}

/* ---- the last screen ---- */

static void summary(const char *name, lv_color_t color, const char *value) {
    lv_obj_t *r = row(name);
    lv_obj_t *v = text(r, &sony_22, color, value);

    lv_obj_set_style_min_height(r, 0, 0);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
}

static void build_done(void) {
    char        s[128], name[40], ip[20];
    const char *kept = clock_kept_by();
    bool        wifi = wifi_get_status() == WIFI_CONNECTED;

    if (wifi_eth_active()) {
        summary("Network", TC(ok), "Ethernet connected");
    } else if (wifi) {
        if (!wifi_network(name, sizeof(name))) {
            snprintf(name, sizeof(name), "a network");
        }
        snprintf(s, sizeof(s), "Connected to %s", name);
        summary("WiFi", TC(ok), s);
    } else {
        summary("WiFi", TC(warn), skipped[STEP_WIFI] ? "Skipped: Settings > Wireless joins a network later"
                                                     : "Not connected: Settings > Wireless joins a network later");
    }
    if (params.callsign.x[0]) {
        snprintf(s, sizeof(s), params.qth.x[0] ? "%s, locator %s" : "%s, no locator", params.callsign.x, params.qth.x);
        summary("Station", TC(ok), s);
    } else {
        summary("Station", TC(warn), skipped[STEP_STATION] ? "Skipped: Settings > General > STATION has the callsign"
                                                           : "No callsign: Settings > General > STATION has it");
    }
    snprintf(s, sizeof(s), "%s, %s", kept ? (strcmp(kept, "NTP") ? "Kept by the GPS" : "Set by NTP")
                                          : "Not set by NTP or a GPS", clock_zones[clock_zone_now()].label);
    summary("Clock", kept ? TC(ok) : TC(text_value), s);

    settings_network_name(name, sizeof(name));
    if (net_address(ip, sizeof(ip))) {
        snprintf(s, sizeof(s), "http://%s.local or http://%s, in a browser on the same network", name, ip);
    } else {
        snprintf(s, sizeof(s), "http://%s.local, in a browser on the same network", name);
    }
    summary("Web configurator", TC(text_value), s);
    summary("Manual", TC(text_value), "github.com/Guru-RF/OpenX6X00, and a PDF with each release");
    summary("This guide", TC(text_dim), "Settings > General > STATION runs it again");
}

/* ---- the window ---- */

static const char *const titles[STEPS] = {"WiFi", "Callsign and locator", "Clock and time zone", "Done"};

static void build(void) {
    lv_obj_t *head;

    if (!shown) {
        return;
    }
    if (body) {
        lv_obj_del(body);
    }
    items_n = 0;
    status = clock_now = clock_date = zone_dd = NULL;
    memset(dt_spin, 0, sizeof(dt_spin));

    body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 796 - 2 * PAD, DIALOG_H - 12);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, PAD, 6);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);    /* a wider font may need more: the focus scrolls to it */
    lv_obj_set_style_text_color(body, TC(text), 0);

    /* "FIRST START  1 / 3", the step's name under it */
    head = row(NULL);
    lv_obj_set_style_min_height(head, 0, 0);
    lv_obj_t *l = text(head, &sony_20, TC(text_head), "FIRST START");
    lv_obj_set_style_text_letter_space(l, 3, 0);
    if (step < STEP_DONE) {
        lv_label_set_text_fmt(text(head, &sony_20, TC(text_dim), ""), "%i / 3", step + 1);
    }
    text(body, &sony_30, TC(text), titles[step]);

    switch (step) {
        case STEP_WIFI:     build_wifi(); break;
        case STEP_STATION:  build_station(); break;
        case STEP_TIME:     build_time(); break;
        default:            build_done(); break;
    }

    /* The keys need a place in the group to come to the guide: the window itself when the step has no control */
    if (!items_n) {
        lv_group_add_obj(keyboard_group, dialog.obj);
    } else {
        lv_group_remove_obj(dialog.obj);
    }
    lv_group_set_editing(keyboard_group, false);
    lv_group_focus_obj(items_n ? items[focus_back < items_n ? focus_back : 0] : dialog.obj);
    focus_back = 0;
    for (int i = 0; i < BUTTONS; i++) {
        buttons_refresh(page.items[i]);
    }
}

static void go(int to) {
    dt_save();
    first_start_esc_reset(&esc);
    step       = to;
    focus_back = 0;
    build();
}

/* Twice a second, as the screen's clock: the lines that follow the radio, the step built again when what it offers
 * changed. The networks are asked for once a second, as the WiFi window does */
static void tick_cb(lv_timer_t *t) {
    static bool odd;
    char        line[96];
    char        grid[7];

    odd = !odd;
    if (typing || (zone_dd && lv_dropdown_is_open(zone_dd))) {
        return;
    }
    switch (step) {
        case STEP_WIFI:
            if (status && odd) {
                bool ok = wifi_line(line, sizeof(line));

                lv_label_set_text(status, line);
                lv_obj_set_style_text_color(status, ok ? TC(ok) : TC(text_value), 0);
            }
            break;

        case STEP_STATION:
            gps_locator(grid);
            if (strcmp(grid, gps_grid)) {   /* a fix came or went, or the radio moved into another square */
                lv_obj_t *f = lv_group_get_focused(keyboard_group);

                for (int i = 0; i < items_n; i++) {
                    if (items[i] == f) {
                        focus_back = i;
                    }
                }
                build();
            }
            break;

        case STEP_TIME:
            if (clock_kept_by() != kept_shown) {
                bool editing = false;

                for (int i = 0; i < 6; i++) {
                    editing |= dt_spin[i] && lv_obj_has_state(dt_spin[i], LV_STATE_EDITED);
                }
                if (!editing) {
                    build();
                    break;
                }
            }
            clock_show();
            dt_show();
            break;

        default:
            break;
    }
}

/* Every close but the WiFi window's in its place sets the mark: finished, skipped, or left (GEN, another app) */
static void mark_done(void) {
    param_i_set(cfg_first_start_done, 1);
}

static void finish(void) {
    finished = true;
    dialog_destruct();
}

static void esc_pressed(void) {
    if (step == STEP_DONE) {
        finish();
    } else if (first_start_esc(&esc, lv_tick_get())) {
        dialog_destruct();
    } else {
        msg_update_text_fmt("Skip the guide?\nPress ESC again to skip it");
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    switch (key) {
        case LV_KEY_ESC:
            if (list_was_open) {
                list_was_open = false;
            } else {
                esc_pressed();
            }
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

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    shown = true;
    if (!detour) {      /* opened anew: from the first step; back from the WiFi window: on it */
        step = STEP_WIFI;
        memset(skipped, 0, sizeof(skipped));
    }
    detour   = false;
    finished = false;
    typing   = KBD_NONE;
    body     = NULL;
    first_start_esc_reset(&esc);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    build();
    tick = lv_timer_create(tick_cb, 500, NULL);
}

static void destruct_cb(void) {
    if (tick) {
        lv_timer_del(tick);
        tick = NULL;
    }
    if (typing) {
        typing = KBD_NONE;
        textarea_window_close();
    }
    dt_save();
    lv_group_remove_obj(dialog.obj);
    shown   = false;
    body    = NULL;
    items_n = 0;
    if (!detour) {
        mark_done();
        if (!finished) {
            msg_schedule_long_text_fmt("Settings > General > STATION\nruns the guide again");
        }
    }
}

/* ---- the bar ---- */

static const char *back_label(void) {
    return step > STEP_WIFI ? "Back" : "";
}

static const char *act1_label(void) {
    switch (step) {
        case STEP_WIFI:     return "Networks";
        case STEP_STATION:  return "Callsign";
        case STEP_TIME:     return "Time zone";
        default:            return "";
    }
}

static const char *act2_label(void) {
    return step == STEP_STATION ? "Locator" : "";
}

static const char *skip_label(void) {
    return step < STEP_DONE ? "Skip" : "";
}

static const char *next_label(void) {
    return step < STEP_DONE ? "Next" : "Done";
}

static void back_press(button_data_t *b) {
    if (!typing && step > STEP_WIFI) {
        go(step - 1);
    }
}

static void act1_press(button_data_t *b) {
    if (typing) {
        return;
    }
    first_start_esc_reset(&esc);
    switch (step) {
        case STEP_WIFI:
            lv_async_call(wifi_open, NULL);
            break;
        case STEP_STATION:
            kbd_open(KBD_CALLSIGN);
            break;
        case STEP_TIME:
            if (zone_dd) {
                lv_group_focus_obj(zone_dd);
                lv_dropdown_open(zone_dd);
                lv_group_set_editing(keyboard_group, true);
            }
            break;
        default:
            break;
    }
}

static void act2_press(button_data_t *b) {
    if (!typing && step == STEP_STATION) {
        first_start_esc_reset(&esc);
        kbd_open(KBD_QTH);
    }
}

static void skip_press(button_data_t *b) {
    if (!typing && step < STEP_DONE) {
        skipped[step] = true;
        go(step + 1);
    }
}

static void next_press(button_data_t *b) {
    if (typing) {
        return;
    }
    if (step < STEP_DONE) {
        skipped[step] = false;
        go(step + 1);
    } else {
        finish();
    }
}

/* ---- at the start ---- */

static bool due(void) {
    return first_start_due(params.callsign.x, param_i_get(cfg_first_start_done), database_unsaved());
}

/* Not over what the start opened itself (Field Ops at boot, the walkie screen): then at the next start */
static void start_cb(lv_timer_t *t) {
    if (!due()) {
        return;
    }
    if (dialog_is_run() || walkie_on() || textarea_window_is_open()) {
        LV_LOG_USER("First start: the screen is taken, the guide at the next start");
        return;
    }
    LV_LOG_USER("First start: no callsign and no guide yet, the guide comes up");
    main_screen_start_app(ACTION_APP_FIRST_START);
}

void first_start_init(void) {
    if (due()) {
        lv_timer_set_repeat_count(lv_timer_create(start_cb, START_MS, NULL), 1);
    }
}

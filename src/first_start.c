/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the first-start guide: six steps over the main screen, the language, a Bluetooth keyboard, WiFi, the
 *  callsign and locator, the country, the clock and its time zone, each with Back, Skip and Next, then a last screen
 *  with what was set and where the manual and the web configurator are. An app window like the others: F1-F5 for its
 *  buttons, the MFK walks its controls and a press chooses, a touch does the same. The radio receives under it all the
 *  while, the main knob tunes and VOL is the volume. It reuses what Settings has: the language list, Settings >
 *  Bluetooth's scan and pairing (bt_dev.h), the WiFi window for the network, the station's checked keyboard entry for
 *  the callsign and locator, the country list (countries.h), the clock's time zones and date and time fields. The
 *  language comes first and is taken at once: the screen is built again in it, the guide with it, so the steps after
 *  it are in that language. The keyboard comes before the first step that types (the WiFi password), so a radio with
 *  only a Bluetooth keyboard needs no USB one (#291). The country is offered from the callsign, and gives the clock
 *  its time zone when none was chosen (the language and country steps after the CX8ANM edition's first-start wizard,
 *  condoruy).
 *
 *  It comes up by itself once (first_start_rule.h): a new card, or the settings reset. Settings > General > STATION >
 *  First-start guide runs it again.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "first_start.h"

#include "first_start_rule.h"
#include "first_start_kbd.h"
#include "band_plan.h"
#include "board.h"
#include "bt_dev.h"
#include "btaudio.h"
#include "button_bar.h"
#include "cfg/cfg_api.h"
#include "clock.h"
#include "countries.h"
#include "ft8/utils.h"
#include "gps.h"
#include "gui_events.h"
#include "gui_styles.h"
#include "i18n.h"
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
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

enum { STEP_LANG, STEP_KEYBOARD, STEP_WIFI, STEP_STATION, STEP_COUNTRY, STEP_TIME, STEP_DONE, STEPS };
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
static bool              detour;        /* gave way to the WiFi window, or to the screen built again in another
                                           language: back on the step, nothing marked */
static bool              finished;      /* Done: closed with nothing more to say */
static first_start_esc_t esc;
static int               typing;        /* the keyboard open for KBD_CALLSIGN or KBD_QTH */
static int               focus_back;    /* the control to focus once the step is built again, its place in items */

static lv_obj_t   *body;                /* the step's content, built again for each step */
static lv_obj_t   *items[ITEMS];        /* its controls, in the order the MFK walks them */
static int         items_n;
static lv_obj_t   *status;              /* the WiFi or clock line */
static lv_obj_t   *clock_now, *clock_date;
static lv_obj_t   *zone_dd, *lang_dd, *country_dd;     /* the step's list */
static lv_obj_t   *dt_spin[6];
static lv_timer_t *tick;

/* What a step was built with: looked at again as the clock ticks, and the step built again when one changed */
static char        gps_grid[7];         /* the locator of a current fix, "" none: the station step offers it */
static char        suggest_shown[3];    /* the country step: the callsign's country offered, "" none */
static const char *kept_shown;          /* the clock step: what keeps the clock */
static bool        zone_by_country;     /* the clock's zone was set from the country in this guide */

static struct tm dt_tm;                 /* the clock step: the date and time fields */
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

/* The WiFi step's line: as the WiFi window says it, with the network and the address once joined */
static bool wifi_line(char *buf, size_t size) {
    char name[33], ip[20];

    if (wifi_eth_active()) {
        net_address(ip, sizeof(ip));
        snprintf(buf, size, _("Ethernet connected, %s: WiFi is not needed"), ip);
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
                snprintf(name, sizeof(name), "%s", _("the network"));
            }
            if (net_address(ip, sizeof(ip))) {
                snprintf(buf, size, _("Connected to %s, %s"), name, ip);
            } else {
                snprintf(buf, size, _("Connected to %s"), name);
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

/* ESC on the open list only closes it (the dropdown does that before the guide's key_cb sees the key) */
static bool list_was_open;

static void list_key_cb(lv_event_t *e) {
    list_was_open = lv_dropdown_is_open(lv_event_get_target(e));
}

/* A list: options, sel chosen, cb on a choice */
static lv_obj_t *dropdown(lv_obj_t *parent, const char *options, uint16_t sel, lv_coord_t w, lv_event_cb_t cb) {
    lv_obj_t *dd = lv_dropdown_create(parent);

    item(dd);
    styles_dropdown_list(lv_dropdown_get_list(dd));
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_symbol(dd, NULL);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_style_text_font(dd, &sony_22, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &sony_22, 0);
    lv_obj_set_style_pad_ver(dd, 8, 0);
    lv_obj_set_size(dd, w, ROW_H - 4);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(dd, list_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    return dd;
}

/* The step's list open: the step is not built again under it */
static bool list_open(void) {
    return (zone_dd && lv_dropdown_is_open(zone_dd)) || (lang_dd && lv_dropdown_is_open(lang_dd))
           || (country_dd && lv_dropdown_is_open(country_dd));
}

/* The step's own button (F2): its list opened, the MFK in it */
static void list_show(lv_obj_t *dd) {
    if (dd) {
        lv_group_focus_obj(dd);
        lv_dropdown_open(dd);
        lv_group_set_editing(keyboard_group, true);
    }
}

/* ---- step 1: the language ---- */

/* As Settings > Display > Language: English and each language file, by its own name, "(draft)" after one not done */
static i18n_lang_t langs[24];
static int         n_langs;

/* The screen built again in the new language, as Settings' row does (theme_apply): the guide goes with it and comes
 * back on this step, in the language */
static void relang(void *arg) {
    if (!shown) {
        return;
    }
    detour = true;
    main_screen_set_theme(params.theme_name.x);
    main_screen_start_app(ACTION_APP_FIRST_START);
}

static void lang_cb(lv_event_t *e) {
    uint16_t i = lv_dropdown_get_selected(lv_event_get_target(e));

    if (i >= n_langs) {
        return;
    }
    first_start_esc_reset(&esc);
    params_str_set(&params.language, langs[i].code);
    params_flush();
    if (!i18n_load(langs[i].code)) {
        msg_update_text_fmt("Language file not found: English");
    }
    lv_async_call(relang, NULL);    /* not from inside the dropdown's own event */
}

/* A name our own fonts cannot draw: a letter beyond Latin-1 (UTF-8 lead bytes from 0xC4 on). The font of such a
 * language comes with it (i18n_font()), so the name shows as boxes until it is chosen */
static void build_lang(void) {
    static char options[sizeof(langs) / sizeof(langs[0]) * 80];
    size_t      n     = 0;
    uint16_t    sel   = 0;
    bool        draft = false;

    para(&sony_22, TC(text), "The language of the screens: this guide, the menus and the apps take it at once. "
                             "Settings > Display has it too.");
    n_langs = i18n_list(langs, sizeof(langs) / sizeof(langs[0]));
    for (int i = 0; i < n_langs && n < sizeof(options) - 80; i++) {
        bool d   = !strcmp(langs[i].status, "draft");
        bool now = !strcmp(langs[i].code, i18n_code());
        char name[80];

        i18n_list_name(&langs[i], name, sizeof(name));   /* 中文 as "Chinese, simplified (zh_CN)" where it would be boxes */
        n += snprintf(options + n, sizeof(options) - n, "%s%s%s", i ? "\n" : "", name, d ? " (draft)" : "");
        draft |= d;
        if (now) {
            sel = i;
        }
    }
    lang_dd = dropdown(row("Language"), options, sel, 400, lang_cb);
    if (draft) {
        para(&sony_20, TC(text_dim), "A language marked draft is still being checked by a speaker.");
    }
}

/* ---- step 2: a Bluetooth keyboard ---- */

#define BTK_REFRESH 4           /* ticks: bt_dev's list asked for again every 2 s, as Settings > Bluetooth does */
#define TURN_ON_MS  30000       /* after Turn on, this long for the adapter to come: the chip, bluetoothd, hci0 */
#define BT_ON       "/usr/sbin/btcat-ctl bt on >/dev/null"    /* as Settings > Bluetooth's switch */
#define BT_FAKE     BOARD_DATA "/first_start_bt.conf"  /* the PC build's screenshots (first_start_kbd.h) */

static void build(void);

static fsk_kept_t btk_kept;             /* the keyboards a scan found, until the next one */
static fsk_in_t   btk_in;               /* what the step was last decided from */
static fsk_out_t  btk_out;              /* and what it shows */
static char       btk_shown[400];       /* the same as a text: the step is built again when it changes */
static bool       btk_scan_asked;       /* Scan pressed on this step */
static bool       btk_pair_asked;       /* a keyboard chosen on this step */
static uint32_t   btk_turn_on_at;       /* Turn on pressed (LVGL's tick), 0 not */
static char       btk_typed[48];        /* what was typed to try the keyboard */
static lv_obj_t  *btk_echo;             /* its line */

/* What Bluetooth and the keyboard say now, and the keyboards found */
static void btk_read(void) {
    bt_dev_t now[BT_DEV_MAX];
    int      n = 0;

#ifdef HOST_BUILD
    if (fsk_fake_load(BT_FAKE, &btk_in, btk_typed[0] ? NULL : btk_typed, sizeof(btk_typed))) {
        return;
    }
#endif
    memset(&btk_in, 0, sizeof(btk_in));
    bt_dev_adapter(&btk_in.adapter);
    btk_in.chip_on    = params.wifi_enabled.x;
    btk_in.turning_on = btk_turn_on_at && lv_tick_elaps(btk_turn_on_at) < TURN_ON_MS;
    btk_in.scanning   = bt_dev_scanning();
    btk_in.busy       = bt_dev_busy();
    btk_in.scan_asked = btk_scan_asked;
    btk_in.pair_asked = btk_pair_asked;
    btk_in.kbd_ready  = keyboard_ready();
    bt_dev_activity(btk_in.activity, sizeof(btk_in.activity));
    while (n < BT_DEV_MAX && bt_dev_get(n, &now[n])) {
        n++;
    }
    btk_in.n = fsk_merge(&btk_kept, now, n, btk_in.devs);
}

/* What the step shows, as a text: the state, and the keyboards listed by their address, name and pairing */
static void btk_signature(char *buf, size_t size) {
    size_t n = snprintf(buf, size, "%i %i %i %s|%s", btk_out.state, btk_out.fail, btk_out.pairing, btk_out.code,
                        btk_out.name);

    for (int i = 0; i < btk_out.n_shown && n < size; i++) {
        const bt_dev_t *d = &btk_in.devs[btk_out.shown[i]];

        n += snprintf(buf + n, size - n, "|%s %s %i", d->addr, d->name, d->paired);
    }
}

/* Bluetooth on and nothing running: Scan can start */
static bool btk_can_scan(void) {
    const bt_adapter_t *a = &btk_in.adapter;

    return a->seen && a->bluez && a->present && a->powered && !btk_in.busy && !btk_in.scanning;
}

static bool btk_can_turn_on(void) {
    return btk_out.state == FSK_CHIP_OFF || btk_out.state == FSK_BT_OFF;
}

/* As Settings > Bluetooth's Scan: 16 s, LE and classic in turns; what an earlier scan found goes */
static void btk_scan(void) {
    if (!btk_can_scan()) {
        return;
    }
    first_start_esc_reset(&esc);
    btk_kept.n     = 0;
    btk_scan_asked = true;
    btk_pair_asked = false;
    bt_dev_scan();
}

/* The chip off: on with WiFi, which the next step wants on too. The adapter off: on as Settings > Bluetooth's switch
 * does it, kept on for the next starts */
static void btk_turn_on(void) {
    if (btk_out.state == FSK_CHIP_OFF) {
        wifi_power_on();
    } else if (btk_out.state == FSK_BT_OFF) {
        sh_run(BT_ON, NULL, NULL);
    } else {
        return;
    }
    first_start_esc_reset(&esc);
    btk_turn_on_at = lv_tick_get() | 1;     /* 0 is "not pressed" */
    bt_dev_refresh();
}

static void btk_scan_cb(lv_event_t *e) {
    btk_scan();
}

static void btk_turn_on_cb(lv_event_t *e) {
    btk_turn_on();
}

/* A keyboard chosen: bt_dev pairs it (its pairing key pressed again for one that takes a new address each time, the
 * code to type shown), trusts it so it comes back by itself at the next starts, and connects it. One paired before
 * is only connected */
static void btk_pick_cb(lv_event_t *e) {
    int i = (intptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    if (i < 0 || i >= btk_out.n_shown || bt_dev_busy()) {
        return;
    }
    first_start_esc_reset(&esc);
    btk_pair_asked = true;
    bt_dev_connect(btk_in.devs[btk_out.shown[i]].addr);
}

/* What bt_dev's worker does with the keyboard chosen */
static const char *btk_pairing_line(void) {
    switch (btk_out.pairing) {
        case FSK_PAIR_KEY:          return "Press the keyboard's pairing key now";
        case FSK_PAIR_SEARCH:       return "Looking for the keyboard...";
        case FSK_PAIR_PAIRING:      return "Pairing...";
        case FSK_PAIR_CONNECTING:   return "Connecting...";
        default:                    return "Bluetooth is busy, a moment...";
    }
}

/* How the pairing failed, and what to do */
static const char *btk_failed_line(void) {
    switch (btk_out.fail) {
        case FSK_FAIL_NOT_FOUND:    return "The keyboard was not seen: press its pairing key, then choose it again";
        case FSK_FAIL_CONNECT:      return "Paired, but not connected: press a key on it, or choose it again";
        default:                    return "Pairing failed: press its pairing key, then choose it again";
    }
}

static void btk_echo_show(void) {
    if (btk_echo) {
        lv_label_set_text_fmt(btk_echo, "%s_", btk_typed);
    }
}

static void build_keyboard(void) {
    char        line[128];
    const char *say   = line;
    lv_color_t  color = TC(text_value);
    bool        list  = false, scan = false;

    btk_read();
    fsk_decide(&btk_in, &btk_out);
    btk_signature(btk_shown, sizeof(btk_shown));
    bt_dev_refresh();

    para(&sony_22, TC(text), "A Bluetooth keyboard can type the WiFi password, the callsign and the locator in the "
                             "next steps. A USB keyboard needs nothing here: it types once plugged in.");
    switch (btk_out.state) {
        case FSK_STARTING:
            say = "Bluetooth starting...";
            break;
        case FSK_NO_SERVICE:
            say   = "Bluetooth is not running on this radio: Skip this step, or plug in a USB keyboard";
            color = TC(warn);
            break;
        case FSK_NO_ADAPTER:
            say   = "No Bluetooth adapter found: Skip this step, or plug in a USB keyboard";
            color = TC(warn);
            break;
        case FSK_CHIP_OFF:
            say = "Bluetooth is off with WiFi, they share a chip: Turn on switches both on";
            break;
        case FSK_BT_OFF:
            say = "Bluetooth is off: Turn on switches it on";
            break;
        case FSK_READY:
            say  = "Put the keyboard in pairing mode, then Scan";
            scan = true;
            break;
        case FSK_SCANNING:
            say  = "Looking for keyboards, 16 s...";
            list = true;
            break;
        case FSK_LIST:
            say  = "Choose your keyboard: the guide pairs it and connects it";
            list = scan = true;
            break;
        case FSK_NONE_FOUND:
            say   = "No keyboard found: is it in pairing mode? Scan looks again";
            color = TC(warn);
            scan  = true;
            break;
        case FSK_PAIRING:
            say = btk_pairing_line();
            break;
        case FSK_PASSKEY:
            say = "Type this code on the keyboard, then its Enter key:";
            break;
        case FSK_FAILED:
            say   = btk_failed_line();
            color = TC(warn);
            list  = scan = true;
            break;
        case FSK_CONNECTED:
            snprintf(line, sizeof(line), _("%s connected: waiting for its keys..."), btk_out.name);
            color = TC(ok);
            break;
        case FSK_WORKS:
            if (btk_out.name[0]) {
                snprintf(line, sizeof(line), _("%s works: type something to try it"), btk_out.name);
            } else {
                say = "A keyboard works: type something to try it";
            }
            color = TC(ok);
            list  = btk_scan_asked && !btk_out.name[0];     /* Scan pressed with a USB keyboard in: what it found */
            break;
    }
    status = para(&sony_24, color, say);

    if (btk_out.state == FSK_PASSKEY) {
        lv_obj_t *l = text(body, &sony_60, TC(accent), btk_out.code);

        lv_obj_set_style_text_letter_space(l, 8, 0);
    } else if (btk_out.state == FSK_WORKS) {
        btk_echo = text(row("Typed"), &sony_24, TC(text_value), "");
        btk_echo_show();
    }
    for (int i = 0; list && i < btk_out.n_shown; i++) {
        const bt_dev_t *d = &btk_in.devs[btk_out.shown[i]];
        lv_obj_t       *r = row(i ? "" : "Keyboards");
        lv_obj_t       *b = button(r, d->name, 340, btk_pick_cb);

        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        text(r, &sony_20, TC(text_dim), d->paired ? "paired: press to connect" : "new: press to pair");
    }
    if (scan && !(list && btk_out.n_shown)) {      /* under a list it would be out of sight: F2 has it */
        button(row(NULL), "Scan", 220, btk_scan_cb);
    } else if (btk_can_turn_on()) {
        button(row(NULL), "Turn on", 220, btk_turn_on_cb);
    }
}

/* Twice a second on the step: built again when what it shows changed, the MFK on the same control */
static void btk_tick(void) {
    static int ticks;
    char       now[sizeof(btk_shown)];

    if (++ticks % BTK_REFRESH == 0) {
        bt_dev_refresh();
    }
    btk_read();
    fsk_decide(&btk_in, &btk_out);
    btk_signature(now, sizeof(now));
    if (strcmp(now, btk_shown)) {
        lv_obj_t *f = lv_group_get_focused(keyboard_group);

        for (int i = 0; i < items_n; i++) {
            if (items[i] == f) {
                focus_back = i;
            }
        }
        build();
    }
}

/* ---- step 3: WiFi ---- */

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

/* ---- step 4: callsign and locator ---- */

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
        snprintf(use, sizeof(use), _("Use %s"), gps_grid);
        button(row("GPS fix"), use, 220, gps_use_cb);
    } else {
        para(&sony_20, TC(text_dim), "With a GPS that has a fix, its locator is offered here.");
    }
}

/* ---- step 5: the country ---- */

/* The callsign's country (countries.h), on a worker: cty.dat is read on the first lookup, about 0.1 s. suggest_for is
 * the callsign asked last, suggest its country ("" none), good once suggest_busy is down again */
static char        suggest_for[16];
static char        suggest[3];
static atomic_bool suggest_busy;

static void *suggest_thread(void *arg) {
    char cc[3];

    country_of_call(arg, cc);
    memcpy(suggest, cc, sizeof(cc));
    free(arg);
    atomic_store(&suggest_busy, false);
    return NULL;
}

/* The country to offer for the callsign as it is now, "" none (yet): asked once a callsign, the guide's tick asks
 * again until the answer is there */
static const char *suggestion(void) {
    pthread_t t;
    char     *call;

    if (atomic_load(&suggest_busy)) {
        return "";
    }
    if (strcmp(suggest_for, params.callsign.x) == 0) {
        return suggest;
    }
    snprintf(suggest_for, sizeof(suggest_for), "%s", params.callsign.x);
    suggest[0] = 0;
    if (!params.callsign.x[0] || !(call = strdup(params.callsign.x))) {
        return "";
    }
    atomic_store(&suggest_busy, true);
    if (pthread_create(&t, NULL, suggest_thread, call) == 0) {
        pthread_detach(t);
    } else {
        free(call);
        atomic_store(&suggest_busy, false);
    }
    return "";
}

/* The clock's zone from the country: the zone of the list with its time (the same string, else the same offsets and
 * rules), when no zone was ever chosen (a new card, a factory reset) or this guide set it from a country before. The
 * clock step shows it, to be changed there */
static void zone_from_country(void) {
    const country_t *c = country_now();
    int              z = -1;

    if (!c || (clock_tz_chosen() && !zone_by_country)) {
        return;
    }
    for (size_t i = 0; i < clock_zones_n && z < 0; i++) {
        if (!strcmp(clock_zones[i].posix, c->tz)) {
            z = i;
        }
    }
    for (size_t i = 0; i < clock_zones_n && z < 0; i++) {
        if (tz_alike(clock_zones[i].posix, c->tz)) {
            z = i;
        }
    }
    if (z >= 0) {
        clock_tz_set(clock_zones[z].posix);
        zone_by_country = true;
    }
}

static void rebuild(void *arg) {
    build();
}

static void country_set(const char *cc) {
    first_start_esc_reset(&esc);
    country_save(cc);
    zone_from_country();
    focus_back = 0;
    lv_async_call(rebuild, NULL);   /* not from inside the list's or the button's own event */
}

static void country_cb(lv_event_t *e) {
    uint16_t         i = lv_dropdown_get_selected(lv_event_get_target(e));
    const country_t *c = i ? country_sorted(i - 1) : NULL;

    country_set(c ? c->cc : "");
}

static void suggest_use_cb(lv_event_t *e) {
    country_set(suggest_shown);
}

/* What the band plan takes now, for the step's line */
static void plan_line(char *buf, size_t size) {
    const char *how;
    char        at[16];
    int         plan   = param_i_get(cfg_band_region);
    int         region = band_plan_auto(&how, at, sizeof(at));

    if (plan != BP_PLAN_AUTO) {
        snprintf(buf, size, _("Band plan: %s, as chosen in Settings > General"), _(band_plan_name(plan)));
    } else if (!strcmp(how, "gps")) {
        snprintf(buf, size, _("Band plan: Auto, Region %i from the GPS"), region);
    } else if (!strcmp(how, "qth")) {
        snprintf(buf, size, _("Band plan: Auto, Region %i from the locator %s"), region, at);
    } else if (!strcmp(how, "country")) {
        snprintf(buf, size, _("Band plan: Auto, Region %i from the country"), region);
    } else if (!strcmp(how, "call")) {
        snprintf(buf, size, _("Band plan: Auto, Region %i from the callsign %s"), region, at);
    } else {
        snprintf(buf, size, _("Band plan: Auto, Region %i: nothing tells where the radio is"), region);
    }
}

static void build_country(void) {
    static char options[8192];
    char        line[128], use[64];
    char        cc[3];
    size_t      n   = 0;
    const char *sug = suggestion();

    para(&sony_22, TC(text), "The country you operate from. The band plan's Auto takes its IARU region when the GPS "
                             "and the locator do not tell, and the clock its time zone when none was chosen. Settings "
                             "> General has it too.");
    n = snprintf(options, sizeof(options), "-");
    for (int i = 0; i < countries_n() && n < sizeof(options) - 64; i++) {
        n += snprintf(options + n, sizeof(options) - n, "\n%s", country_name(country_sorted(i)));
    }
    country_dd = dropdown(row("Country"), "-", 0, 400, country_cb);
    (lv_dropdown_set_options)(country_dd, options);     /* the names are in the language already: past i18n_hook.h */
    lv_dropdown_set_selected(country_dd, country_sorted_index(country_code(cc)) + 1);

    /* The callsign's country, while another one (or none) is set: one press takes it */
    snprintf(suggest_shown, sizeof(suggest_shown), "%s", sug);
    if (sug[0] && strcmp(sug, cc)) {
        snprintf(use, sizeof(use), _("Use %s"), country_name(country_find(sug)));
        button(row("From the callsign"), use, 400, suggest_use_cb);
        if (!cc[0]) {
            focus_back = 1;
        }
    }
    plan_line(line, sizeof(line));
    status = para(&sony_22, cc[0] ? TC(ok) : TC(text_value), line);
}

/* ---- step 6: the clock and its time zone ---- */

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
    zone_by_country = false;    /* chosen here: a country chosen again leaves it */
    clock_tz_set(clock_zones[lv_dropdown_get_selected(lv_event_get_target(e))].posix);
    clock_show();
    dt_show();
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
        snprintf(line, sizeof(line), _("Set by %s: right to the second, nothing to do here"), _(kept_shown));
    } else {
        snprintf(line, sizeof(line), "Not set by NTP or a GPS: set it below, or join WiFi in the WiFi step");
    }
    status = para(&sony_22, kept_shown ? TC(ok) : TC(warn), line);

    for (size_t i = 0; i < clock_zones_n && n < sizeof(options) - 64; i++) {
        n += snprintf(options + n, sizeof(options) - n, "%s%s", i ? "\n" : "", clock_zones[i].label);
    }
    zone_dd = dropdown(row("Time zone"), options, clock_zone_now(), 400, zone_cb);
    if (zone_by_country) {
        para(&sony_20, TC(text_dim), "From the country: choose another zone here if yours differs.");
    }

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
            snprintf(name, sizeof(name), "%s", _("a network"));
        }
        snprintf(s, sizeof(s), _("Connected to %s"), name);
        summary("WiFi", TC(ok), s);
    } else {
        summary("WiFi", TC(warn), skipped[STEP_WIFI] ? "Skipped: Settings > Wireless joins a network later"
                                                     : "Not connected: Settings > Wireless joins a network later");
    }
    if (params.callsign.x[0]) {
        snprintf(s, sizeof(s), params.qth.x[0] ? _("%s, locator %s") : _("%s, no locator"), params.callsign.x,
                 params.qth.x);
        summary("Station", TC(ok), s);
    } else {
        summary("Station", TC(warn), skipped[STEP_STATION] ? "Skipped: Settings > General > STATION has the callsign"
                                                           : "No callsign: Settings > General > STATION has it");
    }
    if (country_now()) {
        snprintf(s, sizeof(s), _("%s, band plan Region %i"), country_name(country_now()), band_plan_region());
        summary("Country", TC(ok), s);
    } else {
        summary("Country", TC(text_dim), skipped[STEP_COUNTRY] ? "Skipped: Settings > General > STATION has it"
                                                               : "Not set: Settings > General > STATION has it");
    }
    snprintf(s, sizeof(s), "%s, %s", kept ? (strcmp(kept, "NTP") ? _("Kept by the GPS") : _("Set by NTP"))
                                          : _("Not set by NTP or a GPS"), clock_zones[clock_zone_now()].label);
    summary("Clock", kept ? TC(ok) : TC(text_value), s);

    /* The keyboard: one whose keys come, by its name when it is a Bluetooth one */
    btk_read();
    fsk_decide(&btk_in, &btk_out);
    if (btk_out.state == FSK_WORKS || btk_out.state == FSK_CONNECTED) {
        if (btk_out.name[0]) {
            snprintf(s, sizeof(s), _("%s, Bluetooth"), btk_out.name);
        }
        summary("Keyboard", TC(ok), btk_out.name[0] ? s : "Connected");
    } else {
        summary("Keyboard", TC(text_dim), skipped[STEP_KEYBOARD] ? "Skipped: Settings > Bluetooth > DEVICES pairs one"
                                                                 : "None: Settings > Bluetooth > DEVICES pairs one");
    }

    settings_network_name(name, sizeof(name));
    if (net_address(ip, sizeof(ip))) {
        snprintf(s, sizeof(s), _("http://%s.local or http://%s, in a browser on the same network"), name, ip);
    } else {
        snprintf(s, sizeof(s), _("http://%s.local, in a browser on the same network"), name);
    }
    summary("Web configurator", TC(text_value), s);
    summary("Manual", TC(text_value), "github.com/Guru-RF/OpenX6X00, and a PDF with each release");
    summary("This guide", TC(text_dim), "Settings > General > STATION runs it again");
}

/* ---- the window ---- */

static const char *const titles[STEPS] = {"Language", "Keyboard", "WiFi", "Callsign and locator", "Country",
                                          "Clock and time zone", "Done"};

static void build(void) {
    lv_obj_t *head;

    if (!shown) {
        return;
    }
    if (body) {
        lv_obj_del(body);
    }
    items_n = 0;
    status = clock_now = clock_date = zone_dd = lang_dd = country_dd = btk_echo = NULL;
    memset(dt_spin, 0, sizeof(dt_spin));

    body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, DIALOG_W - 2 * PAD, DIALOG_H - 12);
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
        lv_label_set_text_fmt(text(head, &sony_20, TC(text_dim), ""), "%i / %i", step + 1, STEP_DONE);
    }
    text(body, &sony_30, TC(text), titles[step]);

    switch (step) {
        case STEP_LANG:     build_lang(); break;
        case STEP_KEYBOARD:      build_keyboard(); break;
        case STEP_WIFI:     build_wifi(); break;
        case STEP_STATION:  build_station(); break;
        case STEP_COUNTRY:  build_country(); break;
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
    if (typing || list_open()) {
        return;
    }
    switch (step) {
        case STEP_KEYBOARD:
            btk_tick();
            break;

        case STEP_WIFI:
            if (status && odd) {
                bool ok = wifi_line(line, sizeof(line));

                lv_label_set_text(status, line);
                lv_obj_set_style_text_color(status, ok ? TC(ok) : TC(text_value), 0);
            }
            break;

        case STEP_STATION:
            gps_locator(grid);
            suggestion();                   /* the callsign's country looked up for the next step */
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

        case STEP_COUNTRY:
            if (strcmp(suggestion(), suggest_shown)) {  /* the callsign's country came */
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
        case LV_KEY_BACKSPACE:
            if (btk_echo && fsk_echo_back(btk_typed)) {
                btk_echo_show();
            }
            break;
        default:
            /* The keyboard step, once a keyboard works: what is typed on it shows */
            if (btk_echo && fsk_echo_add(btk_typed, sizeof(btk_typed), key)) {
                first_start_esc_reset(&esc);
                btk_echo_show();
            }
            break;
    }
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    shown = true;
    if (!detour) {      /* opened anew: from the first step; back from the WiFi window or a new language: on it */
        step = STEP_LANG;
        memset(skipped, 0, sizeof(skipped));
        zone_by_country = false;
        btk_scan_asked  = btk_pair_asked = false;
        btk_kept.n      = 0;
        btk_turn_on_at  = 0;
        btk_typed[0]    = 0;
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
    shown    = false;
    body     = NULL;
    btk_echo = NULL;
    items_n  = 0;
    if (!detour) {
        mark_done();
        if (!finished) {
            msg_schedule_long_text_fmt("Settings > General > STATION\nruns the guide again");
        }
    }
}

/* ---- the bar ---- */

static const char *back_label(void) {
    return step > STEP_LANG ? "Back" : "";
}

static const char *act1_label(void) {
    switch (step) {
        case STEP_LANG:     return "Language";
        case STEP_KEYBOARD:      return btk_can_turn_on() ? "Turn on" : btk_can_scan() ? "Scan" : "";
        case STEP_WIFI:     return "Networks";
        case STEP_STATION:  return "Callsign";
        case STEP_COUNTRY:  return "Country";
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
    if (!typing && step > STEP_LANG) {
        go(step - 1);
    }
}

static void act1_press(button_data_t *b) {
    if (typing) {
        return;
    }
    first_start_esc_reset(&esc);
    switch (step) {
        case STEP_LANG:
            list_show(lang_dd);
            break;
        case STEP_KEYBOARD:
            if (btk_can_turn_on()) {
                btk_turn_on();
            } else {
                btk_scan();
            }
            break;
        case STEP_WIFI:
            lv_async_call(wifi_open, NULL);
            break;
        case STEP_STATION:
            kbd_open(KBD_CALLSIGN);
            break;
        case STEP_COUNTRY:
            list_show(country_dd);
            break;
        case STEP_TIME:
            list_show(zone_dd);
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
    if (step == STEP_COUNTRY) {
        zone_from_country();    /* a country set before the guide: its zone too, when none was chosen */
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

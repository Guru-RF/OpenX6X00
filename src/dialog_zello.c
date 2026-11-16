/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Zello: the radio on a channel of Zello, the push-to-talk voice service on the internet. The window: the
 *  channels, Connect, Talk, the mic to talk with, and the Simplex repeater's switches; the session itself (the helper,
 *  the audio, the link) is zello.c's and voice_net.c's. A channel chosen here starts rootfs/usr/sbin/x6100-zello; closing the window
 *  ends it, unless the Simplex repeater is on (the main screen says so then); nothing connects otherwise.
 *
 *  Talking on Zello never keys the radio: with the window open the PTT key talks on the channel and never reaches the
 *  radio's PTT (voice_ptt.h, screen_main.c); the voice from the hand mic without its own PTT, or a Bluetooth headset's
 *  mic. The Simplex repeater, off by default, is the one thing that keys the radio, under main's TX rules and only
 *  inside the amateur bands (voice_net.c, shared with SVXConnect: one app's link at a time).
 *
 *  The account, the developer key and the channels are set on the web page's Zello page (webserver apps.py); the GUI
 *  reads the channel list only.
 *
 *  After the CX8ANM edition's Zello (condoruy/OpenX6X00-CX8ANM 2cfc9264, bec4fb36, 1aa72f4a).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dialog_zello.h"

#include "board.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cfg/computed_api.h"
#include "events.h"
#include "fonts/font_select.h"
#include "i18n.h"
#include "keyboard.h"
#include "msg_box.h"
#include "params/params.h"
#include "radio.h"
#include "styles.h"
#include "svx.h"
#include "theme.h"
#include "zello.h"

#include <stdio.h>
#include <string.h>

#define CHANNELS_FILE BOARD_DATA "/zello/channels.txt"
#define MAX_CH        20
#define CH_LEN        64

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void conn_cb(button_data_t *b);
static void talk_cb(button_data_t *b);
static void reload_cb(button_data_t *b);
static void via_cb(button_data_t *b);
static void to_rf_cb(button_data_t *b);
static void to_zello_cb(button_data_t *b);
static void id_cb(button_data_t *b);
static void limit_cb(button_data_t *b);
static void pause_cb(button_data_t *b);
static const char *conn_label(void);
static const char *talk_label(void);
static const char *via_label(void);
static const char *to_rf_label(void);
static const char *to_zello_label(void);
static const char *id_label(void);
static const char *limit_label(void);
static const char *pause_label(void);

static button_data_t btn_conn     = {.type = BTN_TEXT_FN, .label_fn = conn_label, .press = conn_cb};
static button_data_t btn_talk     = {.type = BTN_TEXT_FN, .label_fn = talk_label, .press = talk_cb};
static button_data_t btn_reload   = {.type = BTN_TEXT, .label = "Reload\nchannels", .press = reload_cb};
static button_data_t btn_via      = {.type = BTN_TEXT_FN, .label_fn = via_label, .press = via_cb};
static button_data_t btn_to_rf    = {.type = BTN_TEXT_FN, .label_fn = to_rf_label, .press = to_rf_cb};
static button_data_t btn_to_zello = {.type = BTN_TEXT_FN, .label_fn = to_zello_label, .press = to_zello_cb};
static button_data_t btn_id       = {.type = BTN_TEXT_FN, .label_fn = id_label, .press = id_cb};
static button_data_t btn_limit    = {.type = BTN_TEXT_FN, .label_fn = limit_label, .press = limit_cb};
static button_data_t btn_pause    = {.type = BTN_TEXT_FN, .label_fn = pause_label, .press = pause_cb};

static buttons_page_t page_1, page_2, page_3;

static button_data_t btn_page1 = {.type = BTN_TEXT, .label = "(ZELLO 1:3)", .press = button_next_page_cb,
                                  .hold = button_prev_page_cb, .next = &page_2, .prev = &page_3};
static button_data_t btn_page2 = {.type = BTN_TEXT, .label = "(ZELLO 2:3)", .press = button_next_page_cb,
                                  .hold = button_prev_page_cb, .next = &page_3, .prev = &page_1};
static button_data_t btn_page3 = {.type = BTN_TEXT, .label = "(ZELLO 3:3)", .press = button_next_page_cb,
                                  .hold = button_prev_page_cb, .next = &page_1, .prev = &page_2};

static buttons_page_t page_1 = {{&btn_page1, &btn_conn, &btn_talk, &btn_reload}};
static buttons_page_t page_2 = {{&btn_page2, &btn_via, &btn_to_rf, &btn_to_zello, &btn_id}};
static buttons_page_t page_3 = {{&btn_page3, &btn_limit, &btn_pause}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page_1,
};

dialog_t *dialog_zello = &dialog;

static char        channels[MAX_CH][CH_LEN];
static int         channels_n;
static char        last_channel[CH_LEN];    /* the one chosen last, kept while the GUI runs */
static lv_obj_t   *head_lbl, *state_lbl, *talk_lbl, *table;
static lv_timer_t *status_timer;
static zello_status_t zs;                   /* the session as status_cb last read it */

/* ---------- the channels ---------- */

static void load_channels(void) {
    FILE *f = fopen(CHANNELS_FILE, "r");
    char  line[256];

    channels_n = 0;
    if (!f) return;
    while (channels_n < MAX_CH && fgets(line, sizeof(line), f)) {
        char  *s = line;
        size_t n;

        line[strcspn(line, "\r\n")] = 0;
        while (*s == ' ' || *s == '\t') s++;
        n = strlen(s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
        if (n && n < CH_LEN) snprintf(channels[channels_n++], CH_LEN, "%s", s);
    }
    fclose(f);
}

/* ---------- the buttons ---------- */

static const char *conn_label(void) {
    return zs.on ? "Disconnect" : "Connect";
}

static const char *talk_label(void) {
    if (zs.talking) return "Stop\ntalking";
    return param_i_get(cfg_zello_talk_bt) && !zs.btmic ? "No BT\nmic" : "Talk";
}

static const char *via_label(void) {
    return param_i_get(cfg_zello_talk_bt) ? "Talk via:\nBluetooth" : "Talk via:\nHand mic";
}

static const char *to_rf_label(void) {
    if (!param_i_get(cfg_zello_to_rf)) return "Zello>RF:\nOff";
    if (zs.to_rf_stopped) return "Zello>RF:\nStopped";
    return zs.off_band ? "Zello>RF:\nNo ham band" : "Zello>RF:\nOn";
}

static const char *to_zello_label(void) {
    if (!param_i_get(cfg_zello_to_zello)) return "RF>Zello:\nOff";
    if (zs.rx_off_band) return "RF>Zello:\nNo ham band";
    return zs.rx_outside ? "RF>Zello:\nOutside bands" : "RF>Zello:\nOn";
}

static const char *id_label(void) {
    static char buf[32];

    if (!param_i_get(cfg_zello_id)) return "CW ID:\nOff";
    snprintf(buf, sizeof(buf), "CW ID:\n%d min", (int)param_i_get(cfg_zello_id_min));
    return buf;
}

static const char *limit_label(void) {
    static char buf[32];

    snprintf(buf, sizeof(buf), "TX limit:\n%d s", (int)param_i_get(cfg_zello_limit));
    return buf;
}

static const char *pause_label(void) {
    static char buf[32];

    snprintf(buf, sizeof(buf), "Pause:\n%d s", (int)param_i_get(cfg_zello_pause));
    return buf;
}

/* The mic to talk with: the hand mic or a Bluetooth headset's (its headset profile). From the next message */
static void via_cb(button_data_t *b) {
    bool bt = !param_i_get(cfg_zello_talk_bt);

    param_i_set(cfg_zello_talk_bt, bt);
    buttons_refresh(b);
    buttons_refresh(&btn_talk);
    msg_update_text_fmt("%s", bt ? "Zello: talk through a Bluetooth headset's mic" : "Zello: talk through the hand mic");
}

static bool voice_mode(void) {
    int m = cparam_i_get(cfg_cur_mode);

    return m == radio_mode_lsb || m == radio_mode_usb || m == radio_mode_am || m == radio_mode_nfm;
}

/* One app's link at a time: SVXConnect's on, this one is not switched on */
static bool other_link(const char *what) {
    const char *other = vnet_link_taken(&zello_app, &svx_app);

    if (!other) return false;
    msg_update_text_fmt("%s: %s %s", what, other, _("link is on"));
    return true;
}

/* Zello to RF: the channel's messages transmitted on this frequency. Not without a callsign while the ID is on, nor
 * without the squelch, which tells it the frequency is busy */
static void to_rf_cb(button_data_t *b) {
    bool on = !param_i_get(cfg_zello_to_rf);

    if (on && other_link("Zello to RF")) return;
    if (on && param_i_get(cfg_zello_id) && !params.callsign.x[0]) {
        msg_update_text_fmt("Zello to RF: set your callsign first (Settings), the link sends it as its CW ID");
        return;
    }
    if (on && param_i_get(cfg_squelch) == 0) {
        msg_update_text_fmt("Zello to RF: set the squelch (SQL) first, the link keys only while it is closed");
        return;
    }
    param_i_set(cfg_zello_to_rf, on);
    if (on) {
        zello_link_rearm_rf();
        if (!voice_mode()) msg_update_text_fmt("Zello to RF: it transmits in LSB, USB, AM or FM only");
        else msg_update_text_fmt("Zello to RF on: the channel's messages are transmitted on this frequency");
    } else {
        msg_update_text_fmt("Zello to RF off");
    }
    buttons_refresh(b);
}

/* RF to Zello: what the receiver hears with the squelch open goes to the channel. The squelch tells when */
static void to_zello_cb(button_data_t *b) {
    bool on = !param_i_get(cfg_zello_to_zello);

    if (on && other_link("RF to Zello")) return;
    if (on && param_i_get(cfg_squelch) == 0) {
        msg_update_text_fmt("RF to Zello: set the squelch (SQL) first, it tells when a station is on");
        return;
    }
    param_i_set(cfg_zello_to_zello, on);
    msg_update_text_fmt("%s", on ? "RF to Zello on: what the squelch opens on goes to the channel" : "RF to Zello off");
    buttons_refresh(b);
}

/* The CW ID's interval, or off: 60 120 Off 5 10 15 30 60 ... */
static void id_cb(button_data_t *b) {
    static const int steps[] = {5, 10, 15, 30, 60, 90, 120};
    int              n       = sizeof(steps) / sizeof(steps[0]);
    int              cur     = param_i_get(cfg_zello_id_min);

    if (!param_i_get(cfg_zello_id)) {
        param_i_set(cfg_zello_id, 1);
        param_i_set(cfg_zello_id_min, steps[0]);
    } else if (cur >= steps[n - 1]) {
        if (param_i_get(cfg_zello_to_rf)) {
            msg_update_text_fmt("Zello link: the CW ID can be off where your licence lets an unattended station go "
                                "without it");
        }
        param_i_set(cfg_zello_id, 0);
    } else {
        int i = 0;

        while (i < n && steps[i] <= cur) i++;
        param_i_set(cfg_zello_id_min, steps[i]);
    }
    buttons_refresh(b);
}

static void step_param(ParamInt *p, const int *steps, int n) {
    int cur = param_i_get(p), i = 0;

    while (i < n && steps[i] <= cur) i++;
    param_i_set(p, steps[i < n ? i : 0]);
}

static void limit_cb(button_data_t *b) {
    static const int steps[] = {30, 60, 90, 120, 180, 240, 300};

    step_param(cfg_zello_limit, steps, sizeof(steps) / sizeof(steps[0]));
    buttons_refresh(b);
}

static void pause_cb(button_data_t *b) {
    static const int steps[] = {1, 2, 3, 5, 10, 20, 30};

    step_param(cfg_zello_pause, steps, sizeof(steps) / sizeof(steps[0]));
    buttons_refresh(b);
}

/* The buttons again when what they say has changed: a message ended by the helper or the radio's TX, the helper
 * gone, Zello to RF stopped by the radio, a direction outside the amateur bands */
static void refresh_buttons(void) {
    static const char *conn_shown, *talk_shown, *rf_shown, *rx_shown;

    if (conn_label() != conn_shown) {
        conn_shown = conn_label();
        buttons_refresh(&btn_conn);
    }
    if (talk_label() != talk_shown) {
        talk_shown = talk_label();
        buttons_refresh(&btn_talk);
    }
    if (to_rf_label() != rf_shown) {
        rf_shown = to_rf_label();
        buttons_refresh(&btn_to_rf);
    }
    if (to_zello_label() != rx_shown) {
        rx_shown = to_zello_label();
        buttons_refresh(&btn_to_zello);
    }
}

/* ---------- the screen ---------- */

static void show_all(void) {
    if (!table) return;
    lv_table_set_row_cnt(table, channels_n ? channels_n : 1);
    if (!channels_n) lv_table_set_cell_value(table, 0, 0, "No channels: add them on the radio's web page, Zello");
    for (int i = 0; i < channels_n; i++) {
        bool cur = zs.on && !strcmp(channels[i], zs.channel);

        lv_table_set_cell_value_fmt(table, i, 0, "%s %s", cur ? ">" : "  ", channels[i]);
    }
    refresh_buttons();
}

static void show_head(void) {
    bool rf = param_i_get(cfg_zello_to_rf), rx = param_i_get(cfg_zello_to_zello);

    if (!rf && !rx) {
        lv_label_set_text_fmt(head_lbl, "#%06x ZELLO#  #%06x %s#", theme_hex(TC_text_head), theme_hex(TC_text_dim),
                              _("voice channels on the internet, never on the air"));
        return;
    }
    bool        off  = (zs.off_band && rf) || (zs.rx_off_band && rx);
    const char *what = zs.link_show == VNET_SHOW_TX     ? _("on the air: the channel")
                       : zs.link_show == VNET_SHOW_ID     ? _("on the air: CW ID")
                       : zs.link_show == VNET_SHOW_TO_NET ? _("receiver to the channel")
                       : off                              ? _("amateur bands only")
                       : zs.rx_outside && rx              ? _("outside the amateur bands")
                                                          : "";

    lv_label_set_text_fmt(head_lbl, "#%06x ZELLO#  #%06x %s%s%s#  #%06x %s#", theme_hex(TC_text_head),
                          theme_hex(TC_warn), _("SIMPLEX REPEATER"), rf ? " Zello>RF" : "", rx ? " RF>Zello" : "",
                          theme_hex(TC_tx), what);
}

static void status_cb(lv_timer_t *t) {
    (void)t;
    bool was_on = zs.on;
    char was_channel[sizeof(zs.channel)];

    if (!state_lbl) return;
    snprintf(was_channel, sizeof(was_channel), "%s", zs.channel);
    zello_status(&zs);
    if (zs.on != was_on || strcmp(zs.channel, was_channel)) show_all();
    show_head();

    if (zs.state == ZELLO_ERROR) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_err), zs.reason);
    } else if (!zs.on) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              _("Not connected: choose a channel and press the MFK"));
    } else if (zs.state == ZELLO_ONLINE) {
        if (zs.users >= 0)
            lv_label_set_text_fmt(state_lbl, "#%06x %s#  #%06x %d %s#", theme_hex(TC_ok), zs.channel,
                                  theme_hex(TC_text_dim), zs.users, _("online"));
        else
            lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_ok), zs.channel);
    } else if (zs.state == ZELLO_OFFLINE) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s: %s#", theme_hex(TC_warn), zs.channel, zs.reason);
    } else if (zs.state == ZELLO_RETRY) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s (%s %d s)#", theme_hex(TC_warn), zs.reason, _("again in"),
                              zs.retry_s);
    } else {
        lv_label_set_text_fmt(state_lbl, "#%06x %s %s#", theme_hex(TC_text_dim), _("Connecting to"), zs.channel);
    }

    bool bt = param_i_get(cfg_zello_talk_bt);

    if (zs.talking) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(TC_tx),
                              zs.talk_bt ? _("Talking on Zello: speak into the Bluetooth headset")
                                         : _("Talking on Zello: speak into the hand mic"));
        if (zello_mic_quiet()) msg_update_text_fmt("No audio: talking on Zello uses the hand mic");
    } else if (zs.state == ZELLO_ONLINE && zs.talker[0]) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s# #%06x %s#", theme_hex(TC_text_dim), _("Talking now:"),
                              theme_hex(TC_accent), zs.talker);
    } else if (zs.state == ZELLO_ONLINE && bt) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(zs.btmic ? TC_text_dim : TC_warn),
                              zs.btmic ? _("PTT or Talk: speak into the Bluetooth headset")
                                       : _("No Bluetooth mic: a headset on its headset profile"));
    } else if (zs.state == ZELLO_ONLINE) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              _("PTT or Talk: speak into the hand mic, without its own PTT"));
    } else {
        lv_label_set_text(talk_lbl, "");
    }
    refresh_buttons();
}

static int selected(void) {
    uint16_t row = LV_TABLE_CELL_NONE, col = 0;

    if (!table || !channels_n) return -1;
    lv_table_get_selected_cell(table, &row, &col);
    return row == LV_TABLE_CELL_NONE || row >= channels_n ? -1 : row;
}

static void connect_to(const char *channel) {
    snprintf(last_channel, sizeof(last_channel), "%s", channel);
    if (!zello_connect(channel)) msg_update_text_fmt("Zello: the helper could not start");
    status_cb(NULL);
    show_all();
}

static void disconnect(void) {
    zello_disconnect();
    status_cb(NULL);
    show_all();
}

static void choose(int i) {
    if (i < 0) return;
    if (zs.on && !strcmp(channels[i], zs.channel)) disconnect();
    else connect_to(channels[i]);
}

static void conn_cb(button_data_t *b) {
    (void)b;
    if (zs.on) {
        disconnect();
        return;
    }
    int i = selected();
    if (i < 0) {
        msg_update_text_fmt("Zello: add channels on the radio's web page first");
        return;
    }
    connect_to(channels[i]);
}

static void talk_cb(button_data_t *b) {
    (void)b;
    if (zs.talking) zello_talk_stop();
    else zello_talk_start(false);
    status_cb(NULL);
}

static void reload_cb(button_data_t *b) {
    (void)b;
    load_channels();
    show_all();
    msg_update_text_fmt("Zello channels: %d", channels_n);
}

static void pressed_cb(lv_event_t *e) {
    (void)e;
    choose(selected());
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
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

static lv_obj_t *text(lv_coord_t y, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(dialog.obj);

    lv_label_set_recolor(l, true);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 770);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_obj_set_pos(l, 14, y);
    lv_label_set_text(l, "");
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    buttons_load_page(&page_1);
    zello_window_opened();
    zello_status(&zs);

    head_lbl  = text(6, &sony_22);
    state_lbl = text(36, &sony_24);
    talk_lbl  = text(68, &sony_22);

    table = lv_table_create(dialog.obj);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_pos(table, 10, 100);
    lv_obj_set_size(table, 780, DIALOG_H - 108);
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, 770);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(sel_text), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_color(table, TC(sel_bg), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_group_add_obj(keyboard_group, table);
    lv_group_focus_obj(table);
    lv_group_set_editing(keyboard_group, true);

    load_channels();
    if (zs.on) snprintf(last_channel, sizeof(last_channel), "%s", zs.channel);
    if (!last_channel[0] && channels_n) snprintf(last_channel, sizeof(last_channel), "%s", channels[0]);
    show_all();
    for (int i = 0; i < channels_n; i++) {
        if (!strcmp(channels[i], last_channel)) {
            for (int k = 0; k < i; k++) lv_group_send_data(keyboard_group, LV_KEY_DOWN);
            break;
        }
    }

    status_timer = lv_timer_create(status_cb, 250, NULL);
    status_cb(NULL);
}

static void destruct_cb(void) {
    zello_window_closed();
    if (status_timer) lv_timer_del(status_timer);
    status_timer = NULL;
    lv_group_remove_obj(table);
    head_lbl = state_lbl = talk_lbl = table = NULL;
}

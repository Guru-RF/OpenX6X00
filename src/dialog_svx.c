/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - SVXConnect: the radio on an SvxLink reflector's talkgroups over the internet. The window: the
 *  talkgroups watched with their priorities and who talks on each, the recent overs, Connect, Talk, the lock, the mic
 *  to talk with, and the Simplex repeater's switches; the session itself (the helper, the audio, the link) is svx.c's
 *  and voice_net.c's. Connect starts /usr/bin/svxconnect on the reflector set on the web page; closing the window ends
 *  it, unless the Simplex repeater is on (the main screen says so then); nothing connects otherwise.
 *
 *  Talking on the reflector never keys the radio: with the window open the PTT key talks on the selected talkgroup
 *  and never reaches the radio's PTT (voice_ptt.h, screen_main.c); the voice from the hand mic without its own PTT,
 *  or a Bluetooth headset's mic. The Simplex repeater, off by default, is the one thing that keys the radio, under
 *  main's TX rules and only inside the amateur bands (voice_net.c, shared with Zello: one app's link at a time).
 *
 *  The reflector, the callsign, the talkgroups and the certificate's enrolment are on the web page's SVXConnect page
 *  (webserver apps.py); here a talkgroup is chosen (the MFK on its row) and locked.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dialog_svx.h"

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
#include "util.h"
#include "zello.h"

#include <stdio.h>
#include <string.h>

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void conn_cb(button_data_t *b);
static void talk_cb(button_data_t *b);
static void lock_cb(button_data_t *b);
static void via_cb(button_data_t *b);
static void to_rf_cb(button_data_t *b);
static void to_svx_cb(button_data_t *b);
static void id_cb(button_data_t *b);
static void limit_cb(button_data_t *b);
static void pause_cb(button_data_t *b);
static const char *conn_label(void);
static const char *talk_label(void);
static const char *lock_label(void);
static const char *via_label(void);
static const char *to_rf_label(void);
static const char *to_svx_label(void);
static const char *id_label(void);
static const char *limit_label(void);
static const char *pause_label(void);

static button_data_t btn_conn   = {.type = BTN_TEXT_FN, .label_fn = conn_label, .press = conn_cb};
static button_data_t btn_talk   = {.type = BTN_TEXT_FN, .label_fn = talk_label, .press = talk_cb};
static button_data_t btn_lock   = {.type = BTN_TEXT_FN, .label_fn = lock_label, .press = lock_cb};
static button_data_t btn_via    = {.type = BTN_TEXT_FN, .label_fn = via_label, .press = via_cb};
static button_data_t btn_to_rf  = {.type = BTN_TEXT_FN, .label_fn = to_rf_label, .press = to_rf_cb};
static button_data_t btn_to_svx = {.type = BTN_TEXT_FN, .label_fn = to_svx_label, .press = to_svx_cb};
static button_data_t btn_id     = {.type = BTN_TEXT_FN, .label_fn = id_label, .press = id_cb};
static button_data_t btn_limit  = {.type = BTN_TEXT_FN, .label_fn = limit_label, .press = limit_cb};
static button_data_t btn_pause  = {.type = BTN_TEXT_FN, .label_fn = pause_label, .press = pause_cb};

static buttons_page_t page_1, page_2, page_3;

static button_data_t btn_page1 = {.type = BTN_TEXT, .label = "(SVX 1:3)", .press = button_next_page_cb,
                                  .hold = button_prev_page_cb, .next = &page_2, .prev = &page_3};
static button_data_t btn_page2 = {.type = BTN_TEXT, .label = "(SVX 2:3)", .press = button_next_page_cb,
                                  .hold = button_prev_page_cb, .next = &page_3, .prev = &page_1};
static button_data_t btn_page3 = {.type = BTN_TEXT, .label = "(SVX 3:3)", .press = button_next_page_cb,
                                  .hold = button_prev_page_cb, .next = &page_1, .prev = &page_2};

static buttons_page_t page_1 = {{&btn_page1, &btn_conn, &btn_talk, &btn_lock}};
static buttons_page_t page_2 = {{&btn_page2, &btn_via, &btn_to_rf, &btn_to_svx, &btn_id}};
static buttons_page_t page_3 = {{&btn_page3, &btn_limit, &btn_pause}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page_1,
};

dialog_t *dialog_svx = &dialog;

static lv_obj_t    *head_lbl, *state_lbl, *talk_lbl, *table, *recent_lbl;
static lv_timer_t  *status_timer;
static svx_status_t ss;                     /* the session as status_cb last read it */
static uint32_t     rows[SVX_TGS_MAX];      /* the talkgroup on each row of the table */
static int          rows_n;

/* ---------- the buttons ---------- */

static const char *conn_label(void) {
    return ss.v.on ? "Disconnect" : "Connect";
}

static const char *talk_label(void) {
    if (ss.v.talking) return "Stop\ntalking";
    return param_i_get(cfg_svx_talk_bt) && !ss.v.btmic ? "No BT\nmic" : "Talk";
}

static const char *lock_label(void) {
    return ss.h.locked ? "Lock:\nOn" : "Lock:\nOff";
}

static const char *via_label(void) {
    return param_i_get(cfg_svx_talk_bt) ? "Talk via:\nBluetooth" : "Talk via:\nHand mic";
}

static const char *to_rf_label(void) {
    if (!param_i_get(cfg_svx_to_rf)) return "SVX>RF:\nOff";
    if (ss.v.to_rf_stopped) return "SVX>RF:\nStopped";
    return ss.v.off_band ? "SVX>RF:\nNo ham band" : "SVX>RF:\nOn";
}

static const char *to_svx_label(void) {
    if (!param_i_get(cfg_svx_to_svx)) return "RF>SVX:\nOff";
    if (ss.v.rx_off_band) return "RF>SVX:\nNo ham band";
    return ss.v.rx_outside ? "RF>SVX:\nOutside bands" : "RF>SVX:\nOn";
}

static const char *id_label(void) {
    static char buf[32];

    if (!param_i_get(cfg_svx_id)) return "CW ID:\nOff";
    snprintf(buf, sizeof(buf), "CW ID:\n%d min", (int)param_i_get(cfg_svx_id_min));
    return buf;
}

static const char *limit_label(void) {
    static char buf[32];

    snprintf(buf, sizeof(buf), "TX limit:\n%d s", (int)param_i_get(cfg_svx_limit));
    return buf;
}

static const char *pause_label(void) {
    static char buf[32];

    snprintf(buf, sizeof(buf), "Pause:\n%d s", (int)param_i_get(cfg_svx_pause));
    return buf;
}

/* The lock: the talkgroup stays selected whoever talks elsewhere */
static void lock_cb(button_data_t *b) {
    if (!ss.v.connected) {
        msg_update_text_fmt("SVXConnect: %s", _("not on the reflector yet"));
        return;
    }
    svx_lock(!ss.h.locked);
    buttons_refresh(b);
}

/* The mic to talk with: the hand mic or a Bluetooth headset's (its headset profile). From the next over */
static void via_cb(button_data_t *b) {
    bool bt = !param_i_get(cfg_svx_talk_bt);

    param_i_set(cfg_svx_talk_bt, bt);
    buttons_refresh(b);
    buttons_refresh(&btn_talk);
    msg_update_text_fmt("%s", bt ? "SVXConnect: talk through a Bluetooth headset's mic"
                                 : "SVXConnect: talk through the hand mic");
}

static bool voice_mode(void) {
    int m = cparam_i_get(cfg_cur_mode);

    return m == radio_mode_lsb || m == radio_mode_usb || m == radio_mode_am || m == radio_mode_nfm;
}

/* One app's link at a time: Zello's on, this one is not switched on */
static bool other_link(const char *what) {
    const char *other = vnet_link_taken(&svx_app, &zello_app);

    if (!other) return false;
    msg_update_text_fmt("%s: %s %s", what, other, _("link is on"));
    return true;
}

/* SVX to RF: the selected talkgroup's overs transmitted on this frequency. Not without a callsign while the ID is on,
 * nor without the squelch, which tells it the frequency is busy */
static void to_rf_cb(button_data_t *b) {
    bool on = !param_i_get(cfg_svx_to_rf);

    if (on && other_link("SVX to RF")) return;
    if (on && param_i_get(cfg_svx_id) && !params.callsign.x[0]) {
        msg_update_text_fmt("SVX to RF: set your callsign first (Settings), the link sends it as its CW ID");
        return;
    }
    if (on && param_i_get(cfg_squelch) == 0) {
        msg_update_text_fmt("SVX to RF: set the squelch (SQL) first, the link keys only while it is closed");
        return;
    }
    param_i_set(cfg_svx_to_rf, on);
    if (on) {
        svx_link_rearm_rf();
        if (!voice_mode()) msg_update_text_fmt("SVX to RF: it transmits in LSB, USB, AM or FM only");
        else msg_update_text_fmt("SVX to RF on: the talkgroup's overs are transmitted on this frequency");
    } else {
        msg_update_text_fmt("SVX to RF off");
    }
    buttons_refresh(b);
}

/* RF to SVX: what the receiver hears with the squelch open goes to the selected talkgroup. The squelch tells when */
static void to_svx_cb(button_data_t *b) {
    bool on = !param_i_get(cfg_svx_to_svx);

    if (on && other_link("RF to SVX")) return;
    if (on && param_i_get(cfg_squelch) == 0) {
        msg_update_text_fmt("RF to SVX: set the squelch (SQL) first, it tells when a station is on");
        return;
    }
    param_i_set(cfg_svx_to_svx, on);
    msg_update_text_fmt("%s", on ? "RF to SVX on: what the squelch opens on goes to the talkgroup" : "RF to SVX off");
    buttons_refresh(b);
}

/* The CW ID's interval, or off: 5 10 15 30 60 90 120 Off */
static void id_cb(button_data_t *b) {
    static const int steps[] = {5, 10, 15, 30, 60, 90, 120};
    int              n       = sizeof(steps) / sizeof(steps[0]);
    int              cur     = param_i_get(cfg_svx_id_min);

    if (!param_i_get(cfg_svx_id)) {
        param_i_set(cfg_svx_id, 1);
        param_i_set(cfg_svx_id_min, steps[0]);
    } else if (cur >= steps[n - 1]) {
        if (param_i_get(cfg_svx_to_rf)) {
            msg_update_text_fmt("SVX link: the CW ID can be off where your licence lets an unattended station go "
                                "without it");
        }
        param_i_set(cfg_svx_id, 0);
    } else {
        int i = 0;

        while (i < n && steps[i] <= cur) i++;
        param_i_set(cfg_svx_id_min, steps[i]);
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

    step_param(cfg_svx_limit, steps, sizeof(steps) / sizeof(steps[0]));
    buttons_refresh(b);
}

static void pause_cb(button_data_t *b) {
    static const int steps[] = {1, 2, 3, 5, 10, 20, 30};

    step_param(cfg_svx_pause, steps, sizeof(steps) / sizeof(steps[0]));
    buttons_refresh(b);
}

/* The buttons again when what they say has changed */
static void refresh_buttons(void) {
    static const char *conn_shown, *talk_shown, *rf_shown, *rx_shown, *lock_shown;

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
    if (to_svx_label() != rx_shown) {
        rx_shown = to_svx_label();
        buttons_refresh(&btn_to_svx);
    }
    if (lock_label() != lock_shown) {
        lock_shown = lock_label();
        buttons_refresh(&btn_lock);
    }
}

/* ---------- the screen ---------- */

static void ago_text(char *buf, size_t n, uint64_t ms) {
    uint64_t s = ms / 1000;

    if (s < 60) snprintf(buf, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(buf, n, "%um", (unsigned)(s / 60));
    else snprintf(buf, n, "%uh", (unsigned)(s / 3600));
}

/* A cell, only when its text changed (the table is shown again four times a second) */
static void cell(uint16_t row, uint16_t col, const char *txt) {
    const char *was = lv_table_get_cell_value(table, row, col);

    if (!was || strcmp(was, txt)) lv_table_set_cell_value(table, row, col, txt);
}

/* The talkgroups: the selected one marked, its priority as +, who talks on each and for how long */
static void show_tgs(void) {
    uint64_t now = get_time();
    char     line[96], prio[8], who[64], t[16];
    int      n;

    if (!table) return;
    n = ss.v.on ? ss.h.n_tgs : 0;
    if (n != rows_n || !n) lv_table_set_row_cnt(table, n ? n : 1);
    rows_n = n;
    if (!rows_n) {
        cell(0, 0, !ss.set_up ? _("Not set up: the radio's web page, SVXConnect")
                   : ss.v.on  ? _("No talkgroups yet")
                              : _("Connect: the talkgroups come from the web page"));
        cell(0, 1, "");
        return;
    }
    for (int i = 0; i < rows_n; i++) {
        const svx_tg_t     *g = &ss.h.tgs[i];
        const svx_talker_t *k = svx_proto_talker_on(&ss.h, g->id);
        int                 p = g->prio > 3 ? 3 : g->prio;

        rows[i] = g->id;
        memset(prio, '+', (size_t)p);
        prio[p] = 0;
        who[0]  = 0;
        if (k) {
            ago_text(t, sizeof(t), now - k->since_ms);
            snprintf(who, sizeof(who), "%s %s", k->call, t);
        }
        snprintf(line, sizeof(line), "%s TG %u %s %s%s", g->id == ss.h.tg ? ">" : "  ",
                 (unsigned)g->id, prio, g->sw ? "" : _("listen "), g->muted ? _("muted") : "");
        cell(i, 0, line);
        cell(i, 1, who);
    }
}

/* The last overs, the newest first */
static void show_recent(void) {
    uint64_t now = get_time();
    char     text[1024], t[16];
    size_t   n = 0;

    if (!recent_lbl) return;
    n += (size_t)snprintf(text + n, sizeof(text) - n, "#%06x %s#", theme_hex(TC_text_dim), _("Recent"));
    for (int i = 0; i < ss.h.n_recent && i < 9 && n < sizeof(text) - 80; i++) {
        const svx_recent_t *r = &ss.h.recent[i];

        ago_text(t, sizeof(t), now - r->end_ms);
        n += (size_t)snprintf(text + n, sizeof(text) - n, "\n%s  #%06x TG %u  %s, %us#", r->call,
                              theme_hex(TC_text_dim), (unsigned)r->tg, t, (unsigned)r->dur_s);
    }
    if (!ss.v.on || !ss.h.n_recent) {
        snprintf(text + n, sizeof(text) - n, "\n#%06x %s#", theme_hex(TC_text_dim), _("nobody yet"));
    }
    lv_label_set_text(recent_lbl, text);
}

static void show_head(void) {
    bool rf = param_i_get(cfg_svx_to_rf), rx = param_i_get(cfg_svx_to_svx);

    if (!rf && !rx) {
        lv_label_set_text_fmt(head_lbl, "#%06x SVXCONNECT#  #%06x %s#", theme_hex(TC_text_head),
                              theme_hex(TC_text_dim), _("reflector talkgroups on the internet, never on the air"));
        return;
    }
    bool        off  = (ss.v.off_band && rf) || (ss.v.rx_off_band && rx);
    const char *what = ss.v.show == VNET_SHOW_TX     ? _("on the air: the talkgroup")
                       : ss.v.show == VNET_SHOW_ID     ? _("on the air: CW ID")
                       : ss.v.show == VNET_SHOW_TO_NET ? _("receiver to the talkgroup")
                       : off                           ? _("amateur bands only")
                       : ss.v.rx_outside && rx         ? _("outside the amateur bands")
                                                        : "";

    lv_label_set_text_fmt(head_lbl, "#%06x SVXCONNECT#  #%06x %s%s%s#  #%06x %s#", theme_hex(TC_text_head),
                          theme_hex(TC_warn), _("SIMPLEX REPEATER"), rf ? " SVX>RF" : "", rx ? " RF>SVX" : "",
                          theme_hex(TC_tx), what);
}

/* The certificate when it needs the user: none, expired, about to */
static const char *cert_words(void) {
    const char *c = ss.h.cert;

    if (!strcmp(c, "missing")) return _("No certificate: request one on the web page (SVXConnect)");
    if (!strcmp(c, "expired")) return _("Certificate expired: request a new one on the web page");
    if (!strcmp(c, "unreadable")) return _("Certificate unreadable: request a new one on the web page");
    if (!strcmp(c, "not yet valid")) return _("Certificate not valid yet: is the radio's clock right?");
    return NULL;
}

static void show_state(void) {
    const svx_state_t *h = &ss.h;
    const char        *cw;

    if (h->error[0] && ss.v.on) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_err), h->error);
    } else if (!ss.set_up) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_warn),
                              _("Set up the reflector on the radio's web page: SVXConnect"));
    } else if (!ss.v.on) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_text_dim), _("Not connected: press Connect"));
    } else if ((cw = cert_words())) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_err), cw);
    } else if (h->conn == SVX_CONN_CONNECTED) {
        char tg[48];

        if (h->tg) snprintf(tg, sizeof(tg), "TG %u%s", (unsigned)h->tg, h->locked ? _(" locked") : "");
        else snprintf(tg, sizeof(tg), "%s", _("monitoring"));
        if (h->nodes >= 0)
            lv_label_set_text_fmt(state_lbl, "#%06x %s#  #%06x %s#  #%06x %d %s#", theme_hex(TC_ok), h->host,
                                  theme_hex(TC_accent), tg, theme_hex(TC_text_dim), h->nodes, _("nodes"));
        else
            lv_label_set_text_fmt(state_lbl, "#%06x %s#  #%06x %s#", theme_hex(TC_ok), h->host, theme_hex(TC_accent),
                                  tg);
    } else if (h->conn == SVX_CONN_RECONNECTING || h->conn == SVX_CONN_IDLE) {
        lv_label_set_text_fmt(state_lbl, "#%06x %s%s%s#", theme_hex(TC_warn), _("Not connected"), h->why[0] ? ": " : "",
                              h->why);
    } else {
        lv_label_set_text_fmt(state_lbl, "#%06x %s#", theme_hex(TC_text_dim), _("Connecting to the reflector"));
    }
}

static void show_talk(void) {
    const svx_talker_t *k  = svx_proto_talker(&ss.h);
    bool                bt = param_i_get(cfg_svx_talk_bt);
    bool                on = ss.v.connected;

    if (ss.v.talking) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s TG %u: %s#", theme_hex(TC_tx), _("Talking on"),
                              (unsigned)(ss.h.tx ? ss.h.tx_tg : ss.h.tg),
                              ss.v.talk_bt ? _("speak into the Bluetooth headset") : _("speak into the hand mic"));
        if (svx_mic_quiet()) msg_update_text_fmt("No audio: talking on SVXConnect uses the hand mic");
    } else if (on && k) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s# #%06x %s# #%06x TG %u#", theme_hex(TC_text_dim), _("Talking now:"),
                              theme_hex(TC_accent), k->call, theme_hex(TC_text_dim), (unsigned)k->tg);
    } else if (on && !ss.h.tg) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              _("Choose a talkgroup with the MFK to talk on it"));
    } else if (on && bt) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(ss.v.btmic ? TC_text_dim : TC_warn),
                              ss.v.btmic ? _("PTT or Talk: speak into the Bluetooth headset")
                                         : _("No Bluetooth mic: a headset on its headset profile"));
    } else if (on) {
        lv_label_set_text_fmt(talk_lbl, "#%06x %s#", theme_hex(TC_text_dim),
                              _("PTT or Talk: speak into the hand mic, without its own PTT"));
    } else {
        lv_label_set_text(talk_lbl, "");
    }
}

static void status_cb(lv_timer_t *t) {
    (void)t;
    if (!state_lbl) return;
    svx_status(&ss);
    show_head();
    show_state();
    show_talk();
    show_tgs();
    show_recent();
    refresh_buttons();
}

static int selected(void) {
    uint16_t row = LV_TABLE_CELL_NONE, col = 0;

    if (!table || !rows_n) return -1;
    lv_table_get_selected_cell(table, &row, &col);
    return row == LV_TABLE_CELL_NONE || row >= rows_n ? -1 : row;
}

static void conn_cb(button_data_t *b) {
    (void)b;
    if (ss.v.on) {
        svx_disconnect();
    } else if (!svx_set_up()) {
        msg_update_text_fmt("SVXConnect: set up the reflector on the radio's web page first");
    } else if (!svx_connect()) {
        msg_update_text_fmt("SVXConnect: the helper could not start");
    }
    status_cb(NULL);
}

static void talk_cb(button_data_t *b) {
    (void)b;
    if (ss.v.talking) svx_talk_stop();
    else svx_talk_start(false);
    status_cb(NULL);
}

/* The MFK on a row: that talkgroup selected (one only watched cannot be) */
static void pressed_cb(lv_event_t *e) {
    (void)e;
    int i = selected();

    if (i < 0 || !ss.v.connected) return;
    int k = svx_proto_tg_index(&ss.h, rows[i]);

    if (k >= 0 && !ss.h.tgs[k].sw) {
        msg_update_text_fmt("TG %u: %s", (unsigned)rows[i], _("listened to only, not switchable (web page)"));
        return;
    }
    if (ss.h.tg == rows[i]) {
        msg_update_text_fmt("TG %u", (unsigned)rows[i]);
        return;
    }
    svx_select(rows[i]);
    msg_update_text_fmt("TG %u", (unsigned)rows[i]);
}

/* Before the table's own: the MFK's turn (left, right) moves a row, not a cell, so the talkers' column is never the
 * one selected */
static void rows_only_cb(lv_event_t *e) {
    uint32_t *key = lv_event_get_param(e);

    if (*key == LV_KEY_RIGHT) *key = LV_KEY_DOWN;
    else if (*key == LV_KEY_LEFT) *key = LV_KEY_UP;
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
    svx_window_opened();
    svx_status(&ss);

    head_lbl  = text(6, &sony_22);
    state_lbl = text(36, &sony_24);
    talk_lbl  = text(68, &sony_22);

    table = lv_table_create(dialog.obj);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_pos(table, 10, 100);
    lv_obj_set_size(table, 470, DIALOG_H - 108);
    lv_table_set_col_cnt(table, 2);
    lv_table_set_col_width(table, 0, 290);
    lv_table_set_col_width(table, 1, 170);
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
    lv_obj_add_event_cb(table, rows_only_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(table, pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_group_add_obj(keyboard_group, table);
    lv_group_focus_obj(table);
    lv_group_set_editing(keyboard_group, true);

    recent_lbl = lv_label_create(dialog.obj);
    lv_label_set_recolor(recent_lbl, true);
    lv_label_set_long_mode(recent_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(recent_lbl, 300, DIALOG_H - 112);
    lv_obj_set_style_text_font(recent_lbl, &sony_20, 0);
    lv_obj_set_style_text_color(recent_lbl, TC(text), 0);
    lv_obj_set_pos(recent_lbl, 490, 104);

    status_timer = lv_timer_create(status_cb, 250, NULL);
    status_cb(NULL);
}

static void destruct_cb(void) {
    svx_window_closed();
    if (status_timer) lv_timer_del(status_timer);
    status_timer = NULL;
    lv_group_remove_obj(table);
    head_lbl = state_lbl = talk_lbl = table = recent_lbl = NULL;
}

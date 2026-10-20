/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "info_bar.h"

#include "cfg/cfg_api.h"
#include "gui_styles.h"
#include "theme.h"
#include "params/params.h"
#include "pubsub_ids.h"
#include "wifi.h"
#include "gps.h"
#include "scheduler.h"
#include "cluster/client.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

/* Written by btcat: the number of connected Bluetooth CAT clients. */
#ifndef BTCAT_CLIENTS_FILE
#define BTCAT_CLIENTS_FILE "/tmp/btcat.clients"
#endif

#define STATUS_POLL_S   3

#define COLOR_ICON_OFF  theme_hex(TC_icon_off)
#define COLOR_ICON_IDLE theme_hex(TC_text_mute)
#define COLOR_ICON_BT   theme_hex(TC_info)
/* PRE, ATT, ATU on: half see-through focus colour, or solid where the theme says so */
#define PILL_ON_OPA     (theme_shape.focus_solid ? LV_OPA_COVER : LV_OPA_50)

/* The grid cells, six a row: the radio's state, then the status icons */
typedef enum {
    INFO_VFO, INFO_MODE, INFO_AGC, INFO_PRE, INFO_ATT, INFO_ATU,
    INFO_WIFI, INFO_BT, INFO_HEADPHONES, INFO_CAT, INFO_GPS, INFO_CLUSTER,
    INFO_NUM
} info_items_t;

#define ROW_CELLS 6

typedef enum {
    STATUS_OFF = 0,
    STATUS_IDLE,
    STATUS_ON
} status_t;

typedef struct {
    status_t bt;
    status_t headphones;
    status_t cat;
    status_t gps;
    status_t cluster;
} info_status_t;

static lv_obj_t     *obj;
static lv_obj_t     *items[INFO_NUM];

static SubjectInt   *mode_locked;     /* an app holds the mode: shown soft */

static void wifi_state_change_cb(void *s, lv_msg_t *m);
static void wifi_icon_update(void *arg);
static void *status_thread(void *arg);
static void vfo_update(Subject *subj, void *user_data);
static void mode_update(Subject *subj, void *user_data);
static void agc_update(Subject *subj, void *user_data);
static void pill_update(Subject *subj, void *user_data);
static void atu_update(Subject *subj, void *user_data);
static void deck_init(void);

lv_obj_t *info_init(lv_obj_t *parent) {
    /* The cells whose text never changes; VFO, mode and AGC come from their settings */
    static const char *const fixed[INFO_NUM] = {
        [INFO_PRE] = "PRE", [INFO_ATT] = "ATT", [INFO_ATU] = "ATU",
        [INFO_WIFI] = LV_SYMBOL_WIFI, [INFO_BT] = LV_SYMBOL_BLUETOOTH, [INFO_HEADPHONES] = LV_SYMBOL_AUDIO,
        [INFO_CAT] = LV_SYMBOL_KEYBOARD, [INFO_GPS] = LV_SYMBOL_GPS, [INFO_CLUSTER] = "DX",
    };
    bool again = mode_locked != NULL;   /* a theme change: the thread and the subscriptions stay */

    if (!again) mode_locked = subject_i_create(false);

    obj = lv_obj_create(parent);
    lv_obj_add_style(obj, &info_style, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, theme_shape.vision ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    /* Two rows on the same 6 column grid: radio state, then status icons */
    lv_coord_t col_w = lv_obj_get_style_width(obj, 0) - 2 * lv_obj_get_style_border_width(obj, 0)
        - lv_obj_get_style_pad_left(obj, 0) - lv_obj_get_style_pad_right(obj, 0);
    uint8_t cols = theme_shape.vision ? 5 : 6;   /* VISION: the mode is not in the row */
    col_w /= cols;

    lv_obj_t *row;

    for (uint8_t i = 0; i < INFO_NUM; i++) {
        if (i % ROW_CELLS == 0) {
            row = lv_obj_create(obj);
            lv_obj_add_style(row, &info_row_style, 0);
            lv_obj_set_size(row, col_w * cols - 1, theme_shape.vision ? 40 : i ? 26 : 30);
            if (theme_shape.vision && i)
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);   /* VISION: no status icons */
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        }
        lv_obj_t *item = lv_label_create(row);
        lv_obj_set_width(item, col_w - 1);   /* "USB-D" just fits */
        lv_obj_set_style_text_align(item, LV_TEXT_ALIGN_CENTER, 0);
        if (i < INFO_WIFI) {
            lv_obj_add_style(item, &info_item_style, 0);
            lv_obj_set_style_text_color(item, TC(text), 0);
        } else {
            lv_obj_set_style_text_font(item, &lv_font_montserrat_22, 0);
            lv_obj_set_style_text_color(item, lv_color_hex(COLOR_ICON_OFF), 0);
        }
        if (fixed[i]) lv_label_set_text(item, fixed[i]);
        items[i] = item;
    }

    if (theme_shape.vision)
        lv_obj_add_flag(items[INFO_MODE], LV_OBJ_FLAG_HIDDEN);   /* big in the frequency strip */
    lv_obj_set_style_text_font(items[INFO_CLUSTER], &sony_22, 0);
    lv_obj_set_style_text_color(items[INFO_WIFI], lv_color_hex(COLOR_ICON_IDLE), 0);

    /* Each cell follows its settings; the last subscription of each paints it now */
    subject_subscribe_delayed((Subject *)cfg_band_current_vfo, vfo_update, NULL);
    subject_subscribe_delayed_and_notify((Subject *)cfg_band_split, vfo_update, NULL);
    subject_subscribe_delayed((Subject *)cfg_cur_mode, mode_update, NULL);
    subject_subscribe_delayed_and_notify((Subject *)mode_locked, mode_update, NULL);
    subject_subscribe_delayed_and_notify((Subject *)cfg_cur_agc, agc_update, NULL);
    subject_subscribe_delayed_and_notify((Subject *)cfg_cur_pre, pill_update, (void *)(intptr_t)INFO_PRE);
    subject_subscribe_delayed_and_notify((Subject *)cfg_cur_att, pill_update, (void *)(intptr_t)INFO_ATT);
    subject_subscribe_delayed((Subject *)cfg_fg_freq, atu_update, NULL);   /* transverter bands */
    cfg_atu_loaded_subscribe_delayed(atu_update, NULL);
    subject_subscribe_delayed_and_notify((Subject *)cfg_atu_enabled, atu_update, NULL);

    if (again) {
        wifi_icon_update(NULL);
    } else {
        lv_msg_subscribe(MSG_WIFI_STATE_CHANGED, wifi_state_change_cb, NULL);

        pthread_t thread;
        pthread_create(&thread, NULL, status_thread, NULL);
        pthread_detach(thread);
    }

    if (theme_shape.deck) deck_init();
    return obj;
}

/* Deck (Black Metal, Total Black): the box is the two rows on top. First row: ATT PRE | NR NB DNF |
 * AGC | SPL ATU | RIT XIT, grey when off, white when on, then the status icons (the clock is over
 * the box at the right). Second row: the mode, the filter, BW IF STEP, the scope span at the right */
enum { D_ATT, D_PRE, D_NR, D_NB, D_DNF, D_AGC, D_SPL, D_ATU, D_RIT, D_XIT, D_NUM };

static lv_obj_t *deck_ind[D_NUM], *deck_mode, *deck_info, *deck_scope;
static lv_font_t scope_font;     /* the plus-minus sign, the rest from sony_24 */

static void deck_set(int i, bool on) {
    lv_obj_set_style_text_color(deck_ind[i], on ? TC(text) : TC(text_mute), 0);
}

/* Still subscribed after a theme change to another layout: then there's nothing to update */
static void deck_atu_update(Subject *subj, void *user_data) {
    if (!theme_shape.deck) return;
    bool on = param_i_get(cfg_atu_enabled);

    lv_obj_set_style_text_color(deck_ind[D_ATU], !on ? TC(text_mute) : cfg_atu_is_loaded() ? TC(text) : TC(err), 0);
}

static void deck_update(Subject *subj, void *user_data) {
    static const char *const agc[] = {"AGC OFF", "AGC-S", "AGC-F", "AGC-A"};   /* x6100_agc_t order */
    x6100_agc_t a = cparam_i_get(cfg_cur_agc);
    int32_t     bw = cparam_i_get(cfg_cur_filter_bw), step = param_i_get(cfg_mode_freq_step);
    int32_t     half = 50000 / (params.waterfall_zoom.x ? LV_MAX(1, param_i_get(cfg_mode_zoom)) : 1);

    if (!theme_shape.deck) return;
    char        b[16], st[16];

    deck_set(D_ATT, cparam_i_get(cfg_cur_att));
    deck_set(D_PRE, cparam_i_get(cfg_cur_pre));
    deck_set(D_NR, param_i_get(cfg_nr));
    deck_set(D_NB, param_i_get(cfg_nb));
    deck_set(D_DNF, param_i_get(cfg_dnf));
    deck_set(D_SPL, param_i_get(cfg_band_split));
    deck_set(D_RIT, param_i_get(cfg_rit));
    deck_set(D_XIT, param_i_get(cfg_xit));
    lv_label_set_text(deck_ind[D_AGC], a <= x6100_agc_auto ? agc[a] : "AGC");
    lv_obj_set_style_text_color(deck_ind[D_AGC], a == x6100_agc_off ? TC(text_mute) : TC(ok), 0);
    lv_obj_set_style_bg_opa(deck_ind[D_AGC], a == x6100_agc_off ? LV_OPA_TRANSP : LV_OPA_COVER, 0);

    lv_label_set_text(deck_mode, info_params_mode_label_get());
    if (bw >= 1000) snprintf(b, sizeof(b), "%d.%dk", (int)(bw / 1000), (int)(bw % 1000 / 100));
    else snprintf(b, sizeof(b), "%d", (int)bw);
    if (step < 1000) snprintf(st, sizeof(st), "%d Hz", (int)step);
    else if (step % 1000) snprintf(st, sizeof(st), "%d.%d kHz", (int)(step / 1000), (int)(step % 1000 / 100));
    else snprintf(st, sizeof(st), "%d kHz", (int)(step / 1000));
    lv_label_set_text_fmt(deck_info, "BW %s   IF %+d   STEP %s", b, (int)param_i_get(cfg_band_if_shift), st);
    if (half % 1000) lv_label_set_text_fmt(deck_scope, "SCOPE \xC2\xB1%d.%dk", (int)(half / 1000), (int)(half % 1000 / 100));
    else lv_label_set_text_fmt(deck_scope, "SCOPE \xC2\xB1%dk", (int)(half / 1000));
}

static lv_obj_t *deck_row(lv_coord_t y) {
    lv_obj_t *row = lv_obj_create(obj);

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 28);
    lv_obj_set_pos(row, 10, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_text_font(row, &sony_24, 0);
    lv_obj_set_style_text_color(row, TC(text), 0);
    return row;
}

static void deck_init(void) {
    static const char *const names[D_NUM] = {"ATT", "PRE", "NR", "NB", "DNF", "AGC", "SPL", "ATU", "RIT", "XIT"};
    extern const lv_font_t   rcond_pm_20;
    lv_obj_t                *row = deck_row(0), *icons = lv_obj_get_parent(items[INFO_WIFI]);

    /* The grid of the other themes: its state row hidden, the icons row before the clock */
    lv_obj_set_layout(obj, 0);
    lv_obj_add_flag(lv_obj_get_parent(items[INFO_VFO]), LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(icons, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_column(icons, 8, 0);
    lv_obj_align(icons, LV_ALIGN_TOP_RIGHT, -104, 0);
    for (int i = INFO_WIFI; i < INFO_NUM; i++) {
        lv_obj_set_width(items[i], LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(items[i], i == INFO_CLUSTER ? &sony_20 : &lv_font_montserrat_16, 0);
    }

    for (int i = 0; i < D_NUM; i++) {
        if (i == D_NR || i == D_AGC || i == D_SPL || i == D_RIT) {
            lv_obj_t *bar = lv_obj_create(row);   /* a thin bar between the groups */

            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, 1, 14);
            lv_obj_set_style_bg_color(bar, TC(btn_off_text), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        }
        deck_ind[i] = lv_label_create(row);
        lv_label_set_text(deck_ind[i], names[i]);
    }
    lv_obj_set_style_bg_color(deck_ind[D_AGC], TC(ok_bg), 0);
    lv_obj_set_style_pad_hor(deck_ind[D_AGC], 4, 0);
    lv_obj_set_style_pad_ver(deck_ind[D_AGC], 1, 0);

    row = deck_row(28);
    deck_mode = lv_label_create(row);
    lv_obj_set_style_text_color(deck_mode, TC(mode_text), 0);
    lv_obj_set_style_bg_color(deck_mode, TC(mode_bg), 0);
    lv_obj_set_style_bg_opa(deck_mode, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(deck_mode, 4, 0);
    lv_obj_set_style_radius(deck_mode, 2, 0);

    static lv_point_t shape[] = {{0, 17}, {6, 1}, {44, 1}, {50, 17}};   /* the filter's passband */
    lv_obj_t         *filter = lv_line_create(row);

    lv_line_set_points(filter, shape, 4);
    lv_obj_set_style_line_color(filter, TC(info), 0);
    lv_obj_set_style_line_width(filter, 2, 0);
    lv_obj_set_style_pad_hor(filter, 6, 0);
    deck_info = lv_label_create(row);

    scope_font             = rcond_pm_20;
    scope_font.fallback    = &sony_24;
    scope_font.line_height = sony_24.line_height;
    scope_font.base_line   = sony_24.base_line;
    deck_scope = lv_label_create(obj);
    lv_obj_set_style_text_font(deck_scope, &scope_font, 0);
    lv_obj_set_style_text_color(deck_scope, TC(text), 0);
    lv_obj_align(deck_scope, LV_ALIGN_TOP_RIGHT, -10, 30);

    Subject *subj[] = {(Subject *)cfg_cur_att, (Subject *)cfg_cur_pre, (Subject *)cfg_nr, (Subject *)cfg_nb,
                       (Subject *)cfg_dnf, (Subject *)cfg_cur_agc, (Subject *)cfg_band_split, (Subject *)cfg_rit,
                       (Subject *)cfg_xit, (Subject *)cfg_cur_mode, (Subject *)cfg_cur_filter_bw,
                       (Subject *)cfg_band_if_shift, (Subject *)cfg_mode_freq_step, (Subject *)cfg_mode_zoom};
    for (size_t i = 0; i < sizeof(subj) / sizeof(subj[0]); i++) subject_subscribe_delayed(subj[i], deck_update, NULL);
    subject_subscribe_delayed((Subject *)cfg_fg_freq, deck_atu_update, NULL);
    subject_subscribe_delayed((Subject *)cfg_atu_enabled, deck_atu_update, NULL);
    cfg_atu_loaded_subscribe_delayed(deck_atu_update, NULL);
    deck_update(NULL, NULL);
    deck_atu_update(NULL, NULL);
}

/* names[i], "?" past the end of the table */
static const char *name_of(const char *const *names, size_t n, int32_t i) {
    return i >= 0 && (size_t)i < n ? names[i] : "?";
}

const char *info_params_mode_label_get(void) {
    static const char *const names[] = {"LSB", "LSB-D", "USB", "USB-D", "CW", "CW-R", "AM", "NFM"};   /* x6100_mode_t order */

    return name_of(names, sizeof(names) / sizeof(names[0]), cparam_i_get(cfg_cur_mode));
}

const char *info_params_agc(void) {
    static const char *const names[] = {"OFF", "SLOW", "FAST", "AUTO"};   /* x6100_agc_t order */

    return name_of(names, sizeof(names) / sizeof(names[0]), cparam_i_get(cfg_cur_agc));
}

const char *info_params_vfo_label_get(void) {
    bool b = param_i_get(cfg_band_current_vfo) != X6100_VFO_A;

    if (param_i_get(cfg_band_split)) return b ? "SPL-B" : "SPL-A";
    return b ? "VFO-B" : "VFO-A";
}

void info_lock_mode(bool lock) {
    subject_i_set(mode_locked, lock);
}

static void wifi_icon_update(void *arg) {
    wifi_status_t st = wifi_get_status();
    lv_color_t    c  = st == WIFI_CONNECTED ? TC(ok) : st == WIFI_OFF ? TC(icon_off) : lv_color_hex(COLOR_ICON_IDLE);

    lv_obj_set_style_text_color(items[INFO_WIFI], c, 0);
}

/* Sent from NetworkManager's thread: the icon is set on the LVGL one (a theme change may be
 * making items[] again meanwhile) */
static void wifi_state_change_cb(void *s, lv_msg_t *m) {
    scheduler_put_noargs(wifi_icon_update);
}

static void set_icon(info_items_t item, status_t st, uint32_t on_color) {
    uint32_t color = st == STATUS_ON ? on_color : st == STATUS_IDLE ? COLOR_ICON_IDLE : COLOR_ICON_OFF;
    lv_obj_set_style_text_color(items[item], lv_color_hex(color), 0);
}

static void status_update(void *arg) {
    info_status_t *st = arg;

    set_icon(INFO_BT, st->bt, COLOR_ICON_BT);
    set_icon(INFO_HEADPHONES, st->headphones, COLOR_ICON_BT);
    set_icon(INFO_CAT, st->cat, theme_hex(TC_warn));   /* amber: stands out from white text */
    set_icon(INFO_GPS, st->gps, theme_hex(TC_ok));
    set_icon(INFO_CLUSTER, st->cluster, theme_hex(TC_ok));
}

/* Bluetooth: adapter up, any device connected, an A2DP headset among them.
 * The tools are skipped while the adapter is down. */
static void poll_bluetooth(info_status_t *st) {
    int up = 0, con = 0, a2dp = 0;
    FILE *f = popen("up=$(hciconfig hci0 2>/dev/null | grep -c 'UP RUNNING'); echo $up; "
                    "[ \"$up\" = 1 ] && { hcitool con 2>/dev/null | grep -c ACL; "
                    "pactl list sinks short 2>/dev/null | grep -c bluez_sink; }", "r");

    if (f) {
        if (fscanf(f, "%d %d %d", &up, &con, &a2dp) < 3) {
            con = a2dp = 0;
        }
        pclose(f);
    }
    st->bt = !up ? STATUS_OFF : con ? STATUS_ON : STATUS_IDLE;
    st->headphones = up && a2dp ? STATUS_ON : STATUS_OFF;

    /* The file exists while btcat runs */
    int clients = 0;
    f = fopen(BTCAT_CLIENTS_FILE, "r");
    if (f) {
        if (fscanf(f, "%d", &clients) != 1) {
            clients = 0;
        }
        fclose(f);
        st->cat = clients > 0 ? STATUS_ON : up ? STATUS_IDLE : STATUS_OFF;
    } else {
        st->cat = STATUS_OFF;
    }
}

static void *status_thread(void *arg) {
    info_status_t st;
    double lat, lon;
    int age;

    while (true) {
        poll_bluetooth(&st);

        if (gps_last_fix(&lat, &lon, &age) && age < 10) {
            st.gps = STATUS_ON;
        } else {
            st.gps = gps_status() == GPS_STATUS_WORKING ? STATUS_IDLE : STATUS_OFF;
        }

        switch (cluster_client_state(NULL, 0)) {
            case CLUSTER_CONNECTED:
                st.cluster = STATUS_ON;
                break;
            case CLUSTER_STOPPED:
                st.cluster = STATUS_OFF;
                break;
            default:
                st.cluster = STATUS_IDLE;
                break;
        }

        scheduler_put(status_update, &st, sizeof(st));
        sleep(STATUS_POLL_S);
    }
    return NULL;
}


static void vfo_update(Subject *subj, void *user_data) {
    lv_label_set_text(items[INFO_VFO], info_params_vfo_label_get());
}

/* Digital modes in their own colour; a mode an app holds, soft */
static void mode_update(Subject *subj, void *user_data) {
    x6100_mode_t m   = cparam_i_get(cfg_cur_mode);
    bool         dig = m == x6100_mode_lsb_dig || m == x6100_mode_usb_dig;

    lv_label_set_text(items[INFO_MODE], info_params_mode_label_get());
    lv_obj_set_style_text_color(items[INFO_MODE], dig ? TC(mode_dig) : subject_i_get(mode_locked) ? TC(text_soft) : TC(text), 0);
}

static void agc_update(Subject *subj, void *user_data) {
    lv_label_set_text(items[INFO_AGC], info_params_agc());
}

static void pill_paint(lv_obj_t *o, lv_color_t text, lv_color_t bg, lv_opa_t opa) {
    lv_obj_set_style_text_color(o, text, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
}

/* PRE, ATT, ATU: on, a pill in the focus colour; off, the plain text */
static void pill_set(lv_obj_t *o, bool on) {
    if (on) pill_paint(o, TC(focus_text), TC(focus), PILL_ON_OPA);
    else pill_paint(o, TC(pill_off_text), TC(panel), LV_OPA_0);
}

static void pill_update(Subject *subj, void *user_data) {
    info_items_t i = (intptr_t)user_data;

    pill_set(items[i], cparam_i_get(i == INFO_PRE ? cfg_cur_pre : cfg_cur_att));
}

/* ATU: faint on a transverter band (the tuner is not in the path), red when not tuned here */
static void atu_update(Subject *subj, void *user_data) {
    lv_obj_t *o = items[INFO_ATU];

    if (!param_i_get(cfg_atu_enabled))
        pill_set(o, false);
    else if (cfg_transverter_shift_for(cparam_i_get(cfg_fg_freq)))
        pill_paint(o, TC(text_soft), TC(focus), LV_OPA_20);
    else
        pill_paint(o, cfg_atu_is_loaded() ? TC(focus_text) : TC(err_strong), TC(focus), PILL_ON_OPA);
}

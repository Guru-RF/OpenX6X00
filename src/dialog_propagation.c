/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Propagation app
 *
 *  Two views on one map of the world, the station in the middle, night shaded and the grey line
 *  (dawn and dusk, when the low bands carry far) lit:
 *
 *  - Conditions: N0NBH's solar data and band forecast (conditions.c), day and night, the column
 *    of the time it is here marked, and a line on what to try now.
 *  - Beacons: the NCDXF/IARU beacons (beacons.c). The radio listens on their frequencies in CW
 *    and marks each one heard, with its SNR, on the map and in a table of the 18 by 5 bands.
 *    Sweep stays a round (3 min) on each band in turn, Band on one band, Follow goes with the
 *    beacon chosen (MFK) from band to band. Receive only: nothing is sent.
 */

#include "dialog_propagation.h"
#include "screen_light.h"

#include "app_frame.h"
#include "band_keep.h"
#include "beacons.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "conditions.h"
#include "dialog.h"
#include "events.h"
#include "ft8/utils.h"
#include "keyboard.h"
#include "meter.h"
#include "prop_map.h"
#include "qth/qth.h"
#include "radio.h"
#include "station.h"
#include "styles.h"
#include "sun.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAP_W       400
#define PANEL_X     (MAP_W + 10)
#define PANEL_W     (APP_FRAME_W - PANEL_X - 6)
#define HEARD_S     (15 * 60)       /* heard within: shown as heard */
#define TICK_MS     100

typedef enum { VIEW_COND, VIEW_BEACONS } view_t;
typedef enum { FOLLOW_SWEEP, FOLLOW_BAND, FOLLOW_BEACON } follow_t;

static const char *follow_name[] = {"Sweep", "Band", "Follow"};

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void view_cb(button_data_t *b);
static void refresh_cb(button_data_t *b);
static void mode_cb(button_data_t *b);
static void band_cb(button_data_t *b);

static button_data_t btn_cond_page = {.type = BTN_TEXT, .label = "(PROP 1:1)"};
static button_data_t btn_beacons   = {.type = BTN_TEXT, .label = "Beacons", .press = view_cb};
static button_data_t btn_refresh   = {.type = BTN_TEXT, .label = "Refresh", .press = refresh_cb};

static button_data_t btn_bcn_page  = {.type = BTN_TEXT, .label = "(BEACONS)"};
static button_data_t btn_cond      = {.type = BTN_TEXT, .label = "Conditions", .press = view_cb};
static button_data_t btn_mode      = {.type = BTN_TEXT, .label = "Sweep", .press = mode_cb};
static button_data_t btn_band      = {.type = BTN_TEXT, .label = "Band\n20m", .press = band_cb};

static buttons_page_t page_cond    = {.items = {&btn_cond_page, &btn_beacons, &btn_refresh}};
static buttons_page_t page_beacons = {.items = {&btn_bcn_page, &btn_cond, &btn_mode, &btn_band}};

static dialog_t dialog = {
    .run           = false,
    .construct_cb  = construct_cb,
    .destruct_cb   = destruct_cb,
    .key_cb        = key_cb,
    .restore_radio = true,      /* the beacons retune: the frequency and mode back on close */
};

dialog_t *dialog_propagation = &dialog;

static app_frame_t frame;
static lv_timer_t *timer;
static lv_obj_t   *panel_cond, *panel_bcn;
static view_t      view;
static follow_t    follow = FOLLOW_SWEEP;
static int         band_sel;        /* Band */
static int         sel = 13;        /* the beacon chosen (OH2B) */
static int         tuned = -1;      /* the band the radio is on for the beacons, -1 none */
static int32_t     tuned_freq;
static int64_t     hand_round = -1; /* the round the dial was tuned away by hand in, -1 none */
static double      my_lat, my_lon;
static bool        have_qth, scores_kept;
static time_t      drawn_s;

static lv_obj_t *cond_lbl[3], *cond_cells[COND_GROUPS][3], *cond_head[3];
static lv_obj_t *bcn_head[BEACON_BANDS * 2], *bcn_call[BEACONS_N], *bcn_cell[BEACONS_N][BEACON_BANDS];

static int64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_coord_t x, lv_coord_t y, lv_coord_t w) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_color_t rating_color(cond_rating_t r) {
    return r == COND_GOOD ? TC(ok) : r == COND_FAIR ? TC(warn) : r == COND_POOR ? TC(err) : TC(text_faint);
}

static lv_color_t snr_color(int snr) {
    return snr >= 20 ? TC(ok) : snr >= 10 ? TC(warn) : TC(accent);
}

static bool day_here(time_t t) {
    double slat, slon;

    if (!have_qth) return true;
    sun_subsolar(t, &slat, &slon);
    return sun_elevation(slat, slon, my_lat, my_lon) > SUN_GREY_LOW;
}

/* ---- Conditions ---- */

/* What to try: the best rating of the forecast for now here, the higher band of a tie by day, the
 * lower by night (less absorbed at night, the higher ones close) */
static void cond_advice(const conditions_t *c, bool day, char *out, size_t size) {
    int best = -1;

    for (int g = 0; g < COND_GROUPS; g++) {
        cond_rating_t r = c->band[g][day ? 0 : 1];

        if (r == COND_UNKNOWN) continue;
        if (best < 0 || r > c->band[best][day ? 0 : 1] || (day && r == c->band[best][0])) best = g;
    }

    int p = have_qth ? snprintf(out, size, "%s here. ", day ? "Day" : "Night") : 0;

    if (best >= 0)
        p += snprintf(out + p, size - p, "Try %s: %s.", cond_group_name[best],
                      cond_rating_name(c->band[best][day ? 0 : 1]));
    else
        p += snprintf(out + p, size - p, "No forecast yet: WiFi, then Refresh.");
    if (c->k >= 0 && p < (int)size)
        p += snprintf(out + p, size - p, "  K %d, %s%s", c->k, cond_k_word(c->k),
                      c->k >= 5   ? ": high bands poor, polar paths out"
                      : c->k == 4 ? ": noisier, polar paths weak"
                                  : "");
    if (!have_qth && p < (int)size) {
        snprintf(out + p, size - p, "  Your QTH (Settings) for day and night here.");
    } else if (p < (int)size) {
        double slat, slon;
        sun_subsolar(time(NULL), &slat, &slon);
        if (sun_light(sun_elevation(slat, slon, my_lat, my_lon)) == SUN_GREY)
            snprintf(out + p, size - p, "  You are in the grey line: 80 and 40 m reach far now.");
    }
}

static void cond_fill(void) {
    conditions_t c;
    char         buf[160];
    bool         day = day_here(time(NULL));

    conditions_get(&c);
    if (c.sfi >= 0) snprintf(buf, sizeof(buf), "SFI %d   Sunspots %d   A %d", c.sfi, c.ssn, c.a);
    else snprintf(buf, sizeof(buf), "Solar data: none yet");
    lv_label_set_text(cond_lbl[0], buf);
    if (c.k >= 0) snprintf(buf, sizeof(buf), "K %d %s   X-ray %s", c.k, cond_k_word(c.k), c.xray[0] ? c.xray : "-");
    else buf[0] = 0;
    lv_label_set_text(cond_lbl[1], buf);
    /* "Band Closed": "closed" */
    const char *es = strncasecmp(c.es_na, "Band ", 5) ? c.es_na : c.es_na + 5;
    if (c.noise[0]) snprintf(buf, sizeof(buf), "Noise %s   6m Es: %s", c.noise, es[0] ? es : "-");
    else buf[0] = 0;
    lv_label_set_text(cond_lbl[2], buf);

    for (int col = 1; col <= 2; col++) {
        bool now = (col == 1) == day;
        lv_obj_set_style_bg_opa(cond_head[col], now ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(cond_head[col], now ? TC(sel_text) : TC(text_dim), 0);
    }
    for (int g = 0; g < COND_GROUPS; g++) {
        lv_label_set_text(cond_cells[g][0], cond_group_name[g]);
        for (int w = 0; w < 2; w++) {
            lv_label_set_text(cond_cells[g][w + 1], cond_rating_name(c.band[g][w]));
            lv_obj_set_style_text_color(cond_cells[g][w + 1], rating_color(c.band[g][w]), 0);
        }
    }

    /* The source and its time in the header: "26 Oct 2026 2354 GMT" to "2354 GMT" */
    const char *state = conditions_state();
    const char *when  = c.updated;
    int         age   = (int)((time(NULL) - c.fetched) / 3600);

    for (int sp = 0; sp < 3 && strchr(when, ' '); sp++) when = strchr(when, ' ') + 1;
    if (*state) snprintf(buf, sizeof(buf), "N0NBH/hamqsl.com  %s", state);
    else if (c.have && age >= 3) snprintf(buf, sizeof(buf), "N0NBH/hamqsl.com  %d h old", age);
    else snprintf(buf, sizeof(buf), "N0NBH/hamqsl.com  %s", c.have ? when : "");
    lv_label_set_text(frame.head_right, buf);

    cond_advice(&c, day, buf, sizeof(buf));
    lv_label_set_text(frame.status, buf);
}

static void cond_build(lv_obj_t *body) {
    panel_cond = lv_obj_create(body);
    lv_obj_remove_style_all(panel_cond);
    lv_obj_set_pos(panel_cond, PANEL_X, 4);
    lv_obj_set_size(panel_cond, PANEL_W, APP_FRAME_BODY_H - 8);
    lv_obj_clear_flag(panel_cond, LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t y = 0, row = (APP_FRAME_BODY_H - 8) / 8;

    for (int i = 0; i < 3; i++, y += row) cond_lbl[i] = label(panel_cond, &sony_18, 4, y, PANEL_W - 8);
    lv_obj_set_style_text_color(cond_lbl[2], TC(text_dim), 0);

    static const char *head[] = {"Band", "Day", "Night"};
    lv_coord_t         col_x[] = {4, 120, 220}, col_w[] = {110, 90, 90};

    for (int c = 0; c < 3; c++) {
        cond_head[c] = label(panel_cond, &sony_18, col_x[c], y, col_w[c]);
        lv_label_set_text(cond_head[c], head[c]);
        lv_obj_set_style_text_color(cond_head[c], TC(text_dim), 0);
        lv_obj_set_style_bg_color(cond_head[c], TC(sel_bg), 0);
        lv_obj_set_style_pad_left(cond_head[c], 4, 0);
        lv_obj_set_style_radius(cond_head[c], 3, 0);
    }
    y += row;
    for (int g = 0; g < COND_GROUPS; g++, y += row)
        for (int c = 0; c < 3; c++) {
            cond_cells[g][c] = label(panel_cond, &sony_18, col_x[c], y, col_w[c]);
            lv_obj_set_style_pad_left(cond_cells[g][c], 4, 0);
        }
}

/* ---- Beacons ---- */

static int want_band(time_t t) {
    switch (follow) {
        case FOLLOW_SWEEP:  return (int)((t / BEACON_ROUND_S) % BEACON_BANDS);
        case FOLLOW_BAND:   return band_sel;
        case FOLLOW_BEACON: {
            int b = beacon_band_of(sel, t);
            return b >= 0 ? b : tuned >= 0 ? tuned : 0;
        }
    }
    return 0;
}

static void tune_band(int b) {
    tuned      = b;
    tuned_freq = beacon_freq[b];
    hand_round = -1;
    band_keep_tune(tuned_freq);
    cparam_i_set(cfg_fg_freq, tuned_freq);
    cparam_i_set(cfg_cur_mode, radio_mode_cw);
    beacons_retune(now_ms());
}

static void bcn_listen(void) {
    time_t t = time(NULL);
    int    b = want_band(t);

    if (b != tuned || cparam_i_get(cfg_fg_freq) != tuned_freq) {
        /* Tuned away by hand: the slot does not count, and the band is taken back next round (or by Sweep, Band,
         * Follow, the MFK press). In Band the band stays the same: by the round, else it never came back */
        if (b == tuned && hand_round < 0) hand_round = t / BEACON_ROUND_S;
        if (b != tuned || t / BEACON_ROUND_S != hand_round) {
            tune_band(b);
        } else {
            beacons_retune(now_ms());
            return;
        }
    }
    if (radio_get_state() != RADIO_RX) {
        beacons_retune(now_ms());
        return;
    }
    static uint32_t seq;

    if (meter_seq() != seq) {      /* each reading once */
        seq = meter_seq();
        beacons_sample(now_ms(), tuned, meter_get_db());
    }
}

static void bcn_draw_map(time_t t) {
    prop_map_begin(t);

    int on = tuned >= 0 ? beacon_on(tuned, t) : -1;

    for (int i = 0; i < BEACONS_N; i++) {
        const beacon_heard_t *h = beacons_heard(i, tuned >= 0 ? tuned : 0);
        double                lat, lon;

        qth_str_to_pos(beacons[i].grid, &lat, &lon);
        if (i == on && have_qth) prop_map_path(my_lat, my_lon, lat, lon, TC(accent_soft), 1);
        if (h && h->heard && t - h->when < HEARD_S) prop_map_square(lat, lon, 4, snr_color(h->snr), true);
        else if (h && h->snr != BEACON_NONE) prop_map_square(lat, lon, 3, TC(text_dim), false);
        else prop_map_square(lat, lon, 1, TC(text_faint), true);
        if (i == on) prop_map_square(lat, lon, 7, TC(text_head), false);
        if (i == sel) {
            lv_point_t p = prop_map_xy(lat, lon);
            prop_map_square(lat, lon, 9, TC(accent), false);
            prop_map_text(p.x + 10, p.y - 8, &sony_14, TC(text_head), beacons[i].call);
        }
    }
    prop_map_end();
}

static void bcn_fill(time_t t) {
    char buf[200];
    int  on = tuned >= 0 ? beacon_on(tuned, t) : -1;

    for (int b = 0; b < BEACON_BANDS; b++)
        for (int k = 0; k < 2; k++) {
            lv_obj_t *h = bcn_head[k * BEACON_BANDS + b];
            lv_obj_set_style_text_color(h, b == tuned ? TC(text_head) : TC(text_dim), 0);
        }
    for (int i = 0; i < BEACONS_N; i++) {
        lv_obj_set_style_bg_opa(bcn_call[i], i == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(bcn_call[i], i == sel ? TC(sel_text) : i == on ? TC(text_head) : TC(text), 0);
        for (int b = 0; b < BEACON_BANDS; b++) {
            const beacon_heard_t *h    = beacons_heard(i, b);
            lv_obj_t             *cell = bcn_cell[i][b];
            bool                  now  = i == on && b == tuned;

            if (h->heard && t - h->when < HEARD_S) {
                lv_obj_set_style_bg_color(cell, snr_color(h->snr), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(cell, TC(track), 0);
                lv_obj_set_style_bg_opa(cell, h->snr == BEACON_NONE ? LV_OPA_30 : LV_OPA_COVER, 0);
            }
            lv_obj_set_style_border_width(cell, now ? 2 : 0, 0);
        }
    }

    int     ms   = (int)(now_ms() % BEACON_SLOT_MS);
    int64_t left = BEACON_ROUND_S - t % BEACON_ROUND_S;
    int     err;

    app_frame_bar(&frame, ms / 10, theme_hex(TC_accent));
    if (tuned >= 0 && on >= 0)
        lv_label_set_text_fmt(frame.head_left, "BEACONS  %.3f   #%06x %s# %s", tuned_freq / 1e6,
                              theme_hex(TC_text_head), beacons[on].call, beacons[on].where);
    /* The schedule is by the clock: a clock off by seconds names the wrong beacons */
    char clock[40] = "";

    if (!ftx_clock_synced(&err)) snprintf(clock, sizeof(clock), "#%06x clock?#  ", theme_hex(TC_warn));
    if (follow == FOLLOW_SWEEP)
        lv_label_set_text_fmt(frame.head_right, "%sSweep %s  %d:%02d", clock, beacon_band_name[tuned < 0 ? 0 : tuned],
                              (int)(left / 60), (int)(left % 60));
    else
        lv_label_set_text_fmt(frame.head_right, "%s%s %s", clock, follow_name[follow],
                              beacon_band_name[tuned < 0 ? 0 : tuned]);

    /* The chosen one: where, how far, where heard */
    double lat, lon;
    int    p = 0, n = 0;

    qth_str_to_pos(beacons[sel].grid, &lat, &lon);
    p += snprintf(buf + p, sizeof(buf) - p, "%s %s", beacons[sel].call, beacons[sel].where);
    if (have_qth) p += snprintf(buf + p, sizeof(buf) - p, ", %.0f km", qth_pos_dist(my_lat, my_lon, lat, lon));
    for (int b = 0; b < BEACON_BANDS && p < (int)sizeof(buf); b++) {
        const beacon_heard_t *h = beacons_heard(sel, b);
        if (!h->heard || t - h->when >= HEARD_S) continue;
        p += snprintf(buf + p, sizeof(buf) - p, "%s %s %+d dB (heard down to %s), %d min", n++ ? ";" : ":",
                      beacon_band_name[b], h->snr, beacon_power_heard(h->snr), (int)((t - h->when) / 60));
    }
    if (!n) {
        int listened = 0;
        for (int b = 0; b < BEACON_BANDS; b++) listened += beacons_heard(sel, b)->snr != BEACON_NONE;
        snprintf(buf + p, sizeof(buf) - p, "%s",
                 listened ? ": not heard here yet (or off the air)" : ": not listened to yet");
    }
    lv_label_set_text(frame.status, buf);
}

static void bcn_build(lv_obj_t *body) {
    panel_bcn = lv_obj_create(body);
    lv_obj_remove_style_all(panel_bcn);
    lv_obj_set_pos(panel_bcn, PANEL_X, 2);
    lv_obj_set_size(panel_bcn, PANEL_W, APP_FRAME_BODY_H - 4);
    lv_obj_clear_flag(panel_bcn, LV_OBJ_FLAG_SCROLLABLE);

    /* Two columns of nine: the call, then a cell a band; the bands over them by number */
    static const char *num[BEACON_BANDS] = {"20", "17", "15", "12", "10"};
    lv_coord_t col_w = PANEL_W / 2, row = (APP_FRAME_BODY_H - 6) / 10, cell = 18, gap = 3;
    lv_coord_t call_w = col_w - 6 - BEACON_BANDS * (cell + gap);

    for (int k = 0; k < 2; k++)
        for (int b = 0; b < BEACON_BANDS; b++) {
            lv_obj_t *h = label(panel_bcn, &sony_14, k * col_w + call_w + b * (cell + gap), 0, cell + gap);
            lv_label_set_text(h, num[b]);
            bcn_head[k * BEACON_BANDS + b] = h;
        }
    for (int i = 0; i < BEACONS_N; i++) {
        lv_coord_t x = (i / 9) * col_w, y = row * (1 + i % 9);

        bcn_call[i] = label(panel_bcn, &sony_14, x, y + 1, call_w - 4);
        lv_label_set_text(bcn_call[i], beacons[i].call);
        lv_obj_set_style_bg_color(bcn_call[i], TC(sel_bg), 0);
        lv_obj_set_style_pad_left(bcn_call[i], 3, 0);
        lv_obj_set_style_radius(bcn_call[i], 3, 0);
        for (int b = 0; b < BEACON_BANDS; b++) {
            lv_obj_t *c = lv_obj_create(panel_bcn);
            lv_obj_remove_style_all(c);
            lv_obj_set_size(c, cell, row - 5);
            lv_obj_set_pos(c, x + call_w + b * (cell + gap), y + 2);
            lv_obj_set_style_radius(c, 3, 0);
            lv_obj_set_style_border_color(c, TC(text_head), 0);
            bcn_cell[i][b] = c;
        }
    }
}

/* ---- Views, keys, the clock ---- */

static void draw_map(time_t t) {
    if (view == VIEW_BEACONS) {
        bcn_draw_map(t);
    } else {
        double slat, slon;

        prop_map_begin(t);
        sun_subsolar(t, &slat, &slon);
        prop_map_square(slat, slon, 5, TC(warn), true);     /* the sun overhead */
        prop_map_end();
    }
    drawn_s = t;
}

static void show_view(view_t v) {
    view = v;
    if (v == VIEW_BEACONS) {
        lv_obj_add_flag(panel_cond, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(panel_bcn, LV_OBJ_FLAG_HIDDEN);
        buttons_load_page(&page_beacons);
        tuned = -1;
        bcn_listen();
    } else {
        lv_obj_add_flag(panel_bcn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(panel_cond, LV_OBJ_FLAG_HIDDEN);
        buttons_load_page(&page_cond);
        app_frame_bar(&frame, -1, 0);
        lv_label_set_text(frame.head_left, have_qth ? "PROPAGATION" : "PROPAGATION   no QTH set");
        conditions_refresh(false);
        cond_fill();
    }
    draw_map(time(NULL));
}

static void timer_cb(lv_timer_t *t) {
    static int ticks;
    time_t     now = time(NULL);

    bool seen = backlight_seen();   /* the beacons are listened to, and the data fetched, with the screen dark too */

    ticks++;
    if (view == VIEW_BEACONS) {
        bcn_listen();
        if (seen && ticks % 5 == 0) bcn_fill(now);
        if (seen && now != drawn_s && now % 10 == 0) draw_map(now);   /* each slot */
    } else {
        if (seen && ticks % 10 == 0) cond_fill();
        if (seen && now - drawn_s >= 60) draw_map(now);
        if (ticks % 600 == 0) conditions_refresh(false);
    }
}

static void view_cb(button_data_t *b) {
    show_view(view == VIEW_COND ? VIEW_BEACONS : VIEW_COND);
}

static void refresh_cb(button_data_t *b) {
    conditions_refresh(true);
    cond_fill();
}

/* A button or the MFK press: back on the beacon frequency at once after a hand tune */
static void take_back(void) {
    if (tuned >= 0 && cparam_i_get(cfg_fg_freq) != tuned_freq) tuned = -1;
}

static void mode_cb(button_data_t *b) {
    take_back();
    follow        = (follow + 1) % 3;
    btn_mode.label = follow_name[follow];
    buttons_refresh(&btn_mode);
    bcn_listen();
    bcn_fill(time(NULL));
}

static void band_cb(button_data_t *b) {
    static char label[16];

    /* Into Band on the band it is on; once there, the next one */
    band_sel = follow == FOLLOW_BAND ? (band_sel + 1) % BEACON_BANDS : tuned >= 0 ? tuned : 0;
    take_back();
    follow   = FOLLOW_BAND;
    snprintf(label, sizeof(label), "Band\n%s", beacon_band_name[band_sel]);
    btn_band.label = label;
    btn_mode.label = follow_name[follow];
    buttons_refresh(&btn_band);
    buttons_refresh(&btn_mode);
    bcn_listen();
    bcn_fill(time(NULL));
    draw_map(time(NULL));
}

static void select_beacon(int d) {
    sel = (sel + d + BEACONS_N) % BEACONS_N;
    if (view == VIEW_BEACONS) {
        if (follow == FOLLOW_BEACON) bcn_listen();
        bcn_fill(time(NULL));
        draw_map(time(NULL));
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            select_beacon(-1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            select_beacon(1);
            break;
        case LV_KEY_ENTER:   /* the chosen beacon, followed */
            if (view == VIEW_BEACONS) {
                take_back();
                follow         = FOLLOW_BEACON;
                btn_mode.label = follow_name[follow];
                buttons_refresh(&btn_mode);
                bcn_listen();
            } else {
                view_cb(NULL);
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
    }
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    app_frame_create(&frame, dialog.obj);

    lv_obj_t *body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    const char *grid = station_qth();
    have_qth         = grid && strlen(grid) >= 4 && qth_grid_check(grid);
    if (have_qth) qth_str_to_pos(grid, &my_lat, &my_lon);
    else my_lat = my_lon = 0;

    lv_obj_t *map_box = lv_obj_create(body);
    lv_obj_remove_style_all(map_box);
    lv_obj_set_pos(map_box, 1, 1);
    lv_obj_set_size(map_box, MAP_W, APP_FRAME_BODY_H - 2);
    lv_obj_clear_flag(map_box, LV_OBJ_FLAG_SCROLLABLE);
    prop_map_create(map_box, MAP_W, APP_FRAME_BODY_H - 2, have_qth ? my_lat : NAN, have_qth ? my_lon : NAN);

    cond_build(body);
    bcn_build(body);

    band_keep_begin();
    if (!scores_kept) {     /* what was heard stays from one opening to the next */
        beacons_reset();
        scores_kept = true;
    }

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    show_view(VIEW_COND);
    timer = lv_timer_create(timer_cb, TICK_MS, NULL);
}

static void destruct_cb(void) {
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    tuned = -1;
    band_keep_end();
}

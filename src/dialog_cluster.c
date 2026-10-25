/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  DX cluster app: spots from a telnet DX cluster over WiFi, listed by
 *  frequency around the VFO. Tuning the VFO keeps the list centred on it;
 *  the MFK browses the list and a press tunes to the selected spot.
 */

#include "dialog_cluster.h"

#include "cluster/client.h"
#include "cluster/spots.h"

#include "buttons.h"
#include "cfg/cfg_api.h"
#include "dialog.h"
#include "events.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg.h"
#include "params/params.h"
#include "radio.h"
#include "styles.h"
#include "theme.h"
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "fonts/font_select.h"

/* VISION: one size over the other themes here, not VISION_FONT_STEPS (too big for these tables) */
#define VF(n) (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS - 1) : &sony_##n)

#define WIDTH         771
#define HEADER_H      38
#define STATUS_H      30
#define TABLE_Y       (10 + HEADER_H)
#define TABLE_H       (DIALOG_H - TABLE_Y - STATUS_H - 8)
#define TIMER_MS      250
#define NEAR_HZ       3000
#define NEW_MIN       2
#define ROW_VFO       (-1)

#define POP_W         480
#define POP_ROW_H     44
#define POP_HEAD_H    38
#define POP_HINT_H    56
#define POP_MAX_ROWS_H (DIALOG_H - POP_HEAD_H - POP_HINT_H - 24)

#define FONT_MAIN     (VF(30)) /* frequency and call */
#define FONT_SMALL    (VF(24)) /* spotter, comment, time */

#define COLOR_NEAR_BG theme_hex(TC_near_bg)
#define COLOR_SEL_BG  theme_hex(TC_sel_bg)
#define COLOR_NEAR    theme_hex(TC_sel_text)
#define COLOR_VFO     theme_hex(TC_vfo)
#define COLOR_VFO_BG  theme_hex(TC_vfo_bg)
#define COLOR_DIM     theme_hex(TC_text_dim)
#define COLOR_NEW     theme_hex(TC_new)
#define COLOR_ALERT_BG     theme_hex(TC_alert_bg)
#define COLOR_ALERT_BORDER theme_hex(TC_alert)
#define COLOR_ALERT_TEXT   theme_hex(TC_alert_text)

#define ALERT_SHOW_MS   10000
#define ALERT_NEW_MIN   5    /* only fresh spots raise an alert */
#define ALERT_REPEAT_S  600  /* the same call on the same band: once per 10 min */
#define ALERT_SEEN      64
#define ALERTS_SEEN_FILE "/tmp/cluster_alerts_seen" /* pattern|freq_hz|utc_min, for the web page */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void close_cb(button_data_t *data);
static void band_all_cb(button_data_t *data);
static void tune_cb(button_data_t *data);
static void mode_cb(button_data_t *data);
static const char *mode_label(void);
static const char *band_label(void);
static void order_cb(button_data_t *data);
static const char *order_label(void);
static void link_cb(button_data_t *data);
static const char *link_label(void);
static void server_cb(button_data_t *data);
static bool popup_key(uint32_t key);
static void use_server(int i);
static void alert_scan(void);

static buttons_page_t btn_page_1, btn_page_2;

static button_data_t btn_page = {
    .type = BTN_TEXT, .label = "(CLUSTER\n1:2)", .press = button_next_page_cb, .next = &btn_page_2};
static button_data_t btn_page2 = {
    .type = BTN_TEXT, .label = "(CLUSTER\n2:2)", .press = button_next_page_cb, .next = &btn_page_1};
static button_data_t btn_server = {.type = BTN_TEXT, .label = "Server", .press = server_cb};
static button_data_t btn_band = {.type = BTN_TEXT_FN, .label_fn = band_label, .press = band_all_cb};
static button_data_t btn_mode = {.type = BTN_TEXT_FN, .label_fn = mode_label, .press = mode_cb};
static button_data_t btn_order = {.type = BTN_TEXT_FN, .label_fn = order_label, .press = order_cb};
static button_data_t btn_link = {.type = BTN_TEXT_FN, .label_fn = link_label, .press = link_cb};
static button_data_t btn_tune = {.type = BTN_TEXT, .label = "Tune", .press = tune_cb};
static button_data_t btn_close = {.type = BTN_TEXT, .label = "Close", .press = close_cb};

static buttons_page_t btn_page_1 = {
    .items = {&btn_page, &btn_band, &btn_tune, &btn_mode, &btn_close},
};
static buttons_page_t btn_page_2 = {
    .items = {&btn_page2, &btn_server, &btn_order, &btn_link, &btn_close},
};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .key_cb       = key_cb,
    .btn_page     = &btn_page_1,
};

dialog_t *dialog_cluster = &dialog;

static lv_obj_t   *head_left, *head_mid, *head_right;
static lv_obj_t   *table;
static lv_obj_t   *status_left, *status_right;
static lv_timer_t *timer;
static lv_obj_t   *popup; /* server list, NULL when closed */
static cluster_server_t srv[CLUSTER_MAX_SERVERS];
static lv_obj_t        *srv_rows[CLUSTER_MAX_SERVERS];
static int              srv_n, srv_sel;

static cluster_spot_t all_spots[CLUSTER_MAX_SPOTS];
static int            all_count;
static uint32_t       spots_gen;

static cluster_spot_t view[CLUSTER_MAX_SPOTS]; /* shown spots, by frequency */
static int            view_count;
static int16_t        rows[CLUSTER_MAX_SPOTS + 1]; /* index into view[], or ROW_VFO */
static int            row_count;

static int      sel = -1; /* index into view[] */
static bool     follow_vfo = true;
static uint8_t  view_mode;  /* quick view filter (CLUSTER_FM_*, 0 = all), not saved */
static int      scope_count; /* spots of the band (or all) before the filters */
static const cluster_filter_t *flt;
static uint32_t filter_gen;
static int32_t  last_vfo;
static int      last_band = -2;
static uint16_t now_min;
static char     server[128];
static int      server_port;
static time_t   conf_mtime;
static char     client_call[sizeof(params.callsign.x)];
static bool     wifi_was_up;

/* Reads the server and (re)starts the client: a running one reconnects
 * only when the server or the callsign changed. */
static void load_server(void) {
    char login[CLUSTER_CALL_LEN];

    conf_mtime = cluster_config_mtime();
    cluster_config_load(server, sizeof(server), &server_port);
    snprintf(client_call, sizeof(client_call), "%s", params.callsign.x);
    cluster_config_login(login, sizeof(login), client_call);
    if (client_call[0] && login[0] && cluster_config_enabled()) cluster_client_start(server, server_port, login);
    else cluster_client_stop();
}

/* Keeps the client running with the current server and callsign, also
 * while the app is closed. */
static void background_cb(lv_timer_t *t) {
    (void)t;
    if (cluster_config_mtime() != conf_mtime || strcmp(client_call, params.callsign.x) != 0) load_server();
    bool up = net_connected();
    if (up && !wifi_was_up) cluster_client_retry();
    wifi_was_up = up;
    alert_scan();
}

void dialog_cluster_start_client(void) {
    load_server();
    lv_timer_create(background_cb, 2000, NULL);
}

static uint16_t utc_now_min(void) {
    time_t    now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
}

/* ---- Alerts: a popup on the main screen for a new spot matching one ---- */

static lv_obj_t      *alert_box;
static lv_timer_t    *alert_timer;
static cluster_spot_t alert_spot;
static cluster_spot_t bg_spots[CLUSTER_MAX_SPOTS];
static uint32_t       bg_gen, bg_filter_gen;
static struct {
    char   dx[CLUSTER_CALL_LEN];
    int    band;
    time_t t;
} seen[ALERT_SEEN];
static struct {
    char    pattern[32];
    int32_t freq_hz;
    uint16_t utc_min;
} last_hit[CLUSTER_MAX_ALERTS];
static int last_hit_n;

static void alert_hide(void) {
    if (alert_timer) {
        lv_timer_del(alert_timer);
        alert_timer = NULL;
    }
    if (alert_box) {
        lv_obj_del(alert_box);
        alert_box = NULL;
    }
}

static void alert_timer_cb(lv_timer_t *t) {
    (void)t;
    alert_timer = NULL; /* one-shot: LVGL deletes it */
    if (alert_box) {
        lv_obj_del(alert_box);
        alert_box = NULL;
    }
}

static void alert_show(const cluster_spot_t *s) {
    static const char *const mode_name[] = {"CW", "LSB", "USB", "DIG", "AM", "FM"};

    alert_hide();
    alert_spot = *s;
    alert_box  = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(alert_box);
    lv_obj_set_pos(alert_box, 120, 318);
    lv_obj_set_size(alert_box, 420, 78);
    lv_obj_set_style_bg_color(alert_box, TC(alert_box), 0);
    lv_obj_set_style_bg_opa(alert_box, 240, 0);
    lv_obj_set_style_border_color(alert_box, lv_color_hex(COLOR_ALERT_BORDER), 0);
    lv_obj_set_style_border_width(alert_box, 2, 0);
    lv_obj_set_style_radius(alert_box, theme_shape.radius_popup, 0);
    lv_obj_clear_flag(alert_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(alert_box);
    lv_obj_set_style_text_font(title, VF(18), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ALERT_TEXT), 0);
    lv_label_set_text(title, "CLUSTER ALERT");
    lv_obj_set_pos(title, 14, 8);

    lv_obj_t *hint = lv_label_create(alert_box);
    lv_obj_set_style_text_font(hint, VF(18), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text(hint, "MFK: tune");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -14, 8);

    lv_obj_t *line = lv_label_create(alert_box);
    lv_obj_set_style_text_font(line, FONT_MAIN, 0);
    lv_obj_set_style_text_color(line, TC(text), 0);
    lv_label_set_long_mode(line, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(line, 420 - 28);
    lv_label_set_text_fmt(line, "%s  %d.%03d.%d  %s", s->dx, s->freq_hz / 1000000, (s->freq_hz / 1000) % 1000,
                          (s->freq_hz % 1000) / 100, mode_name[cluster_guess_mode(s)]);
    lv_obj_set_pos(line, 14, 34);

    alert_timer = lv_timer_create(alert_timer_cb, ALERT_SHOW_MS, NULL);
    lv_timer_set_repeat_count(alert_timer, 1);
}

/* MFK press while the popup is shown: tune to the spot. */
bool dialog_cluster_alert_press(void) {
    if (!alert_box) return false;
    cluster_spot_t s = alert_spot;
    alert_hide();
    dialog_cluster_tune_spot(&s);
    return true;
}

/* Last spot seen for each alert pattern, for the web page. */
static void write_last_hit(const cluster_spot_t *s, const char *pattern) {
    int i;
    for (i = 0; i < last_hit_n; i++)
        if (strcmp(last_hit[i].pattern, pattern) == 0) break;
    if (i == last_hit_n) {
        if (last_hit_n == CLUSTER_MAX_ALERTS) return;
        last_hit_n++;
    }
    snprintf(last_hit[i].pattern, sizeof(last_hit[i].pattern), "%s", pattern);
    last_hit[i].freq_hz = s->freq_hz;
    last_hit[i].utc_min = s->utc_min;

    FILE *f = fopen(ALERTS_SEEN_FILE, "w");
    if (!f) return;
    for (i = 0; i < last_hit_n; i++)
        fprintf(f, "%s|%d|%02d:%02d\n", last_hit[i].pattern, (int)last_hit[i].freq_hz, last_hit[i].utc_min / 60,
                last_hit[i].utc_min % 60);
    fclose(f);
}

/* True once per call and band in ALERT_REPEAT_S. */
static bool alert_is_new(const cluster_spot_t *s, time_t now) {
    int band = cluster_band_of(s->freq_hz), free_i = 0;

    for (int i = 0; i < ALERT_SEEN; i++) {
        if (seen[i].t && now - seen[i].t < ALERT_REPEAT_S && seen[i].band == band &&
            strcasecmp(seen[i].dx, s->dx) == 0)
            return false;
        if (seen[i].t < seen[free_i].t) free_i = i; /* the oldest (or an unused) entry */
    }
    snprintf(seen[free_i].dx, sizeof(seen[free_i].dx), "%s", s->dx);
    seen[free_i].band = band;
    seen[free_i].t    = now;
    return true;
}

/* Looks at the new spots, also while the app is closed. */
static void alert_scan(void) {
    const cluster_filter_t *f = cluster_filter();
    if (f->gen != bg_filter_gen) {
        bg_filter_gen = f->gen;
        bg_gen        = 0; /* alerts changed: look at the spots again */
    }
    int n = cluster_client_snapshot(bg_spots, CLUSTER_MAX_SPOTS, &bg_gen);
    if (n <= 0 || f->alert_count == 0) return;

    time_t   now = time(NULL);
    uint16_t min = utc_now_min();
    int      hit = -1;
    for (int i = 0; i < n; i++) {
        int a = cluster_alert_match(f, &bg_spots[i]);
        if (a < 0 || cluster_age_min(bg_spots[i].utc_min, min) > ALERT_NEW_MIN) continue;
        if (!alert_is_new(&bg_spots[i], now)) continue;
        hit = i;
        write_last_hit(&bg_spots[i], f->alerts[a].pattern);
    }
    /* The newest one is shown; there is no speaker beep (nothing safe to reuse:
     * the speaker path is the TX audio path while transmitting). */
    if (hit >= 0 && f->popup) alert_show(&bg_spots[hit]);
}

static int vfo_band(void) {
    return cluster_band_of(cparam_i_get(cfg_fg_freq));
}

/* All bands when asked, or when the VFO is outside the amateur bands.
 * Band and order are shared with the column on the main screen. */
static bool showing_all(void) {
    return param_i_get(cfg_cluster_all_bands) || vfo_band() < 0;
}

static bool by_time(void) {
    return param_i_get(cfg_cluster_order);
}

static int cmp_freq(const void *a, const void *b) {
    const cluster_spot_t *x = a, *y = b;
    return (x->freq_hz > y->freq_hz) - (x->freq_hz < y->freq_hz);
}

/* Newest first */
static int cmp_age(const void *a, const void *b) {
    const cluster_spot_t *x = a, *y = b;
    return cluster_age_min(x->utc_min, now_min) - cluster_age_min(y->utc_min, now_min);
}

static int nearest_to_vfo(void) {
    int32_t vfo  = cparam_i_get(cfg_fg_freq);
    int     best = -1;
    for (int i = 0; i < view_count; i++)
        if (best < 0 || abs(view[i].freq_hz - vfo) < abs(view[best].freq_hz - vfo)) best = i;
    return best;
}

static int row_of_spot(int idx) {
    for (int r = 0; r < row_count; r++)
        if (rows[r] == idx) return r;
    return 0;
}

/* Top of row r, from the first row. */
static lv_coord_t row_y(int r) {
    lv_table_t *t = (lv_table_t *)table;
    lv_coord_t  y = 0;
    for (int i = 0; i < r && i < row_count; i++) y += t->row_h[i];
    return y;
}

/* Puts row r in the middle of the list. The table is padded by half its
 * height at both ends, so the first and last rows can be centred too. */
static void scroll_to_row(int r) {
    lv_table_t *t = (lv_table_t *)table;
    lv_obj_scroll_to_y(table, row_y(r) + (r < row_count ? t->row_h[r] : 0) / 2, LV_ANIM_OFF);
}

/* Scrolls only when row r is not fully visible. */
static void scroll_to_show_row(int r) {
    lv_table_t *t   = (lv_table_t *)table;
    lv_coord_t  pad = lv_obj_get_style_pad_top(table, LV_PART_MAIN);
    lv_coord_t  top = pad + row_y(r) - lv_obj_get_scroll_y(table);
    if (top < 0 || top + t->row_h[r] > lv_obj_get_height(table)) scroll_to_row(r);
}

static void rebuild(void) {
    cluster_spot_t keep;
    bool           had_sel = sel >= 0 && sel < view_count;
    if (had_sel) keep = view[sel];

    int band = vfo_band();
    flt = cluster_filter();
    view_count  = 0;
    scope_count = 0;
    for (int i = 0; i < all_count; i++) {
        if (!showing_all() && cluster_band_of(all_spots[i].freq_hz) != band) continue;
        scope_count++;
        if (!cluster_filter_pass(flt, &all_spots[i], now_min)) continue;
        if (view_mode && cluster_spot_mode_class(&all_spots[i]) != view_mode) continue;
        view[view_count++] = all_spots[i];
    }
    qsort(view, (size_t)view_count, sizeof(view[0]), by_time() ? cmp_age : cmp_freq);

    int32_t vfo = cparam_i_get(cfg_fg_freq);
    row_count   = 0;
    bool marker = by_time(); /* newest first: no VFO row */
    for (int i = 0; i < view_count; i++) {
        if (!marker && view[i].freq_hz >= vfo) {
            rows[row_count++] = ROW_VFO;
            marker            = true;
        }
        rows[row_count++] = (int16_t)i;
    }
    if (!marker) rows[row_count++] = ROW_VFO;

    sel = -1;
    if (follow_vfo) {
        sel = by_time() ? (view_count ? 0 : -1) : nearest_to_vfo();
    } else if (had_sel) {
        for (int i = 0; i < view_count; i++)
            if (strcasecmp(view[i].dx, keep.dx) == 0 && view[i].freq_hz == keep.freq_hz) sel = i;
        if (sel < 0) sel = nearest_to_vfo();
    }

    lv_table_set_row_cnt(table, (uint16_t)(row_count ? row_count : 1));
    for (int r = 0; r < row_count; r++) lv_table_set_cell_value(table, (uint16_t)r, 0, " ");
    if (by_time()) {
        /* The newest at the top of the list (past the padding) */
        if (follow_vfo || sel < 0) lv_obj_scroll_to_y(table, lv_obj_get_style_pad_top(table, LV_PART_MAIN), LV_ANIM_OFF);
        else scroll_to_show_row(row_of_spot(sel));
    } else {
        /* Following the VFO, its row stays in the middle and the spots move. */
        scroll_to_row(follow_vfo || sel < 0 ? row_of_spot(ROW_VFO) : row_of_spot(sel));
    }
    lv_obj_invalidate(table);
}

static void update_header(void) {
    int32_t vfo = cparam_i_get(cfg_fg_freq);
    lv_label_set_text_fmt(head_left, "CLUSTER #%06x %s# - %s", theme_hex(TC_text), server,
                          params.callsign.x[0] ? params.callsign.x : "no callsign");
    lv_label_set_text_fmt(head_mid, "#%06x %s# - VFO #%06x %d.%03d.%02d#", theme_hex(TC_text),
                          showing_all() ? "All" : cluster_band_name(vfo_band()), theme_hex(TC_text), vfo / 1000000,
                          (vfo / 1000) % 1000, (vfo % 1000) / 10);
    bool conf_filtered = flt && (flt->modes != CLUSTER_FM_ALL || flt->max_age || flt->skip[0]);
    if (view_mode || conf_filtered) {
        static const char *const names[] = {"Filtered", "CW only", "SSB only", "", "DIG only"};
        lv_label_set_text_fmt(head_right, "%s - %d/%d - %02d:%02dZ", names[view_mode], view_count,
                              scope_count, now_min / 60, now_min % 60);
    } else {
        lv_label_set_text_fmt(head_right, "%d spots - %02d:%02dZ", view_count, now_min / 60, now_min % 60);
    }
}

static void update_status(void) {
    char            detail[64];
    cluster_state_t st = cluster_client_state(detail, sizeof(detail));

    if (!params.callsign.x[0]) {
        lv_label_set_text_fmt(status_right, "#%06x Set your callsign: Settings > General#", theme_hex(TC_err));
        return;
    }
    if (!cluster_config_enabled()) {
        lv_label_set_text(status_right, "Cluster link off");
        return;
    }
    if (!net_connected()) {
        lv_label_set_text_fmt(status_right, "#%06x No network: WiFi or Ethernet#", theme_hex(TC_err));
        return;
    }
    switch (st) {
        case CLUSTER_CONNECTED:
            lv_label_set_text_fmt(status_right, "#%06x %s - cluster connected#", theme_hex(TC_ok), wifi_eth_active() ? "Ethernet" : "WiFi");
            break;
        case CLUSTER_CONNECTING:
            lv_label_set_text_fmt(status_right, "#%06x Connecting...#", theme_hex(TC_warn));
            break;
        case CLUSTER_FAILED:
            lv_label_set_text_fmt(status_right, "#%06x %s, %s#", theme_hex(TC_err), detail[0] ? detail : "Cluster error",
                                  strncmp(detail, "refused", 7) == 0 || strncmp(detail, "logged in", 9) == 0
                                      ? "retry in 30 min" : "retrying");
            break;
        default:
            lv_label_set_text(status_right, "Stopped");
            break;
    }
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    bool changed = false;

    int n = cluster_client_snapshot(all_spots, CLUSTER_MAX_SPOTS, &spots_gen);
    if (n >= 0) {
        all_count = n;
        changed   = true;
    }

    int32_t vfo  = cparam_i_get(cfg_fg_freq);
    int     band = vfo_band();
    if (vfo != last_vfo) {
        /* The user is tuning: the list follows the VFO again. */
        follow_vfo = true;
        last_vfo   = vfo;
        changed    = true;
    }
    if (band != last_band) {
        last_band = band;
        changed   = true;
        if (buttons_get_cur_page() == &btn_page_1) buttons_refresh(&btn_band);
    }

    uint16_t m = utc_now_min();
    if (m != now_min) {
        now_min = m;
        changed = true; /* "new" marks age */
    }

    /* The server can be changed from the web configurator too, and the filters. */
    if (cluster_config_mtime() != conf_mtime) load_server();
    if (cluster_filter() != flt || cluster_filter()->gen != filter_gen) {
        filter_gen = cluster_filter()->gen;
        changed    = true;
    }

    if (changed) rebuild();
    update_header();
    update_status();
}

static void table_draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t r = dsc->id;
    if (r >= (uint32_t)row_count) return;

    dsc->rect_dsc->bg_opa   = LV_OPA_COVER;
    dsc->rect_dsc->bg_color = TC(panel);
    if (rows[r] == ROW_VFO) {
        dsc->rect_dsc->bg_color = lv_color_hex(COLOR_VFO_BG);
        return;
    }

    const cluster_spot_t *s = &view[rows[r]];
    if (rows[r] == sel) {
        dsc->rect_dsc->bg_color     = lv_color_hex(COLOR_SEL_BG);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 2;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_FULL;
    } else if (flt && cluster_alert_match(flt, s) >= 0) {
        dsc->rect_dsc->bg_color     = lv_color_hex(COLOR_ALERT_BG);
        dsc->rect_dsc->border_color = lv_color_hex(COLOR_ALERT_BORDER);
        dsc->rect_dsc->border_width = 1;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = LV_BORDER_SIDE_FULL;
    } else if (abs(s->freq_hz - cparam_i_get(cfg_fg_freq)) <= NEAR_HZ) {
        dsc->rect_dsc->bg_color = lv_color_hex(COLOR_NEAR_BG);
    }
}

static void draw_text(lv_obj_draw_part_dsc_t *dsc, const lv_font_t *font, lv_coord_t x1, lv_coord_t x2,
                      lv_color_t color, lv_text_align_t align, const char *text) {
    lv_draw_label_dsc_t label = *dsc->label_dsc;
    label.font                = font;
    label.color               = color;
    label.align               = align;
    lv_area_t area            = *dsc->draw_area;
    area.y1 += lv_obj_get_style_pad_top(table, LV_PART_ITEMS);
    area.x2 = dsc->draw_area->x1 + x2;
    area.x1 = dsc->draw_area->x1 + x1;
    /* One line, cut at the field's right edge (no wrapping into the next row). */
    label.flag |= LV_TEXT_FLAG_EXPAND;
    const lv_area_t *old_clip = dsc->draw_ctx->clip_area;
    lv_area_t        clip;
    if (_lv_area_intersect(&clip, old_clip, &area)) {
        dsc->draw_ctx->clip_area = &clip;
        lv_draw_label(dsc->draw_ctx, &label, &area, text, NULL);
        dsc->draw_ctx->clip_area = old_clip;
    }
}

/* The fields are drawn at fixed x positions within the one table column. */
static void table_draw_end_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t r = dsc->id;
    if (r >= (uint32_t)row_count) return;

    char text[32];
    if (rows[r] == ROW_VFO) {
        int32_t vfo = cparam_i_get(cfg_fg_freq);
        lv_draw_rect_dsc_t line;
        lv_draw_rect_dsc_init(&line);
        line.bg_color  = lv_color_hex(COLOR_VFO);
        lv_area_t area = *dsc->draw_area;
        area.y2        = area.y1 + 1;
        lv_draw_rect(dsc->draw_ctx, &line, &area);
        snprintf(text, sizeof(text), "> %d.%03d.%02d  VFO", vfo / 1000000, (vfo / 1000) % 1000, (vfo % 1000) / 10);
        draw_text(dsc, FONT_MAIN, 8, WIDTH - 8, lv_color_hex(COLOR_VFO), LV_TEXT_ALIGN_LEFT, text);
        return;
    }

    const cluster_spot_t *s    = &view[rows[r]];
    bool                  near = abs(s->freq_hz - cparam_i_get(cfg_fg_freq)) <= NEAR_HZ || rows[r] == sel;
    uint16_t              age  = cluster_age_min(s->utc_min, now_min);
    bool                  alert = flt && cluster_alert_match(flt, s) >= 0;

    snprintf(text, sizeof(text), "%d.%03d.%d", s->freq_hz / 1000000, (s->freq_hz / 1000) % 1000,
             (s->freq_hz % 1000) / 100);
    /* VISION's bigger type: no spotter, the room to the others */
    bool       v     = theme_shape.vision;
    lv_coord_t call  = v ? 176 : 136, comm = v ? 380 : 410, time_x = v ? 660 : 690;

    draw_text(dsc, FONT_MAIN, 0, v ? 164 : 122, TC(text_value), LV_TEXT_ALIGN_RIGHT, text);
    draw_text(dsc, FONT_MAIN, call, v ? 360 : 290,
              alert ? lv_color_hex(COLOR_ALERT_TEXT) : near ? lv_color_hex(COLOR_NEAR) : TC(text), LV_TEXT_ALIGN_LEFT,
              s->dx);
    if (age <= NEW_MIN) {
        lv_coord_t w = lv_txt_get_width(s->dx, strlen(s->dx), FONT_MAIN, 0, 0);
        draw_text(dsc, FONT_MAIN, call + w + 4, v ? 376 : 310, lv_color_hex(COLOR_NEW), LV_TEXT_ALIGN_LEFT, "*");
    }
    if (!v) draw_text(dsc, FONT_SMALL, 300, 400, lv_color_hex(COLOR_DIM), LV_TEXT_ALIGN_LEFT, s->spotter);
    draw_text(dsc, FONT_SMALL, comm, alert ? time_x - 110 : time_x - 10, TC(text_value), LV_TEXT_ALIGN_LEFT, s->comment);
    if (alert)
        draw_text(dsc, FONT_SMALL, time_x - 106, time_x - 4, lv_color_hex(COLOR_ALERT_BORDER), LV_TEXT_ALIGN_RIGHT,
                  "ALERT");
    snprintf(text, sizeof(text), "%02d%02dZ", s->utc_min / 60, s->utc_min % 60);
    draw_text(dsc, FONT_SMALL, time_x, WIDTH - 12, lv_color_hex(COLOR_DIM), LV_TEXT_ALIGN_RIGHT, text);
}

/* The last spot tuned here (MFK on the column or the Cluster app), for a log prefill. Read from any
 * thread: ponytail: no lock, a torn call costs one log prefill */
static char     tuned_call[16];
static uint32_t tuned_seq;

uint32_t dialog_cluster_tuned(char *call, size_t size) {
    snprintf(call, size, "%s", tuned_call);
    return tuned_seq;
}

void dialog_cluster_tune_spot(const cluster_spot_t *s) {
    radio_mode_t mode;

    snprintf(tuned_call, sizeof(tuned_call), "%s", s->dx);
    tuned_seq++;
    switch (cluster_guess_mode(s)) {
        case CLUSTER_MODE_CW:   mode = radio_mode_cw; break;
        case CLUSTER_MODE_LSB:  mode = radio_mode_lsb; break;
        case CLUSTER_MODE_DIGI: mode = radio_mode_usb_dig; break;
        case CLUSTER_MODE_AM:   mode = radio_mode_am; break;
        case CLUSTER_MODE_FM:   mode = radio_mode_nfm; break;
        default:                mode = radio_mode_usb; break;
    }
    main_screen_set_freq(s->freq_hz);
    cparam_i_set(cfg_cur_mode, mode);
    msg_update_text_fmt("%s %d.%03d.%d", s->dx, s->freq_hz / 1000000, (s->freq_hz / 1000) % 1000,
                        (s->freq_hz % 1000) / 100);
}

static void tune_to(int idx) {
    if (idx < 0 || idx >= view_count) return;
    dialog_cluster_tune_spot(&view[idx]);
    follow_vfo = true;
}

static void move_selection(int step) {
    if (view_count == 0) return;
    follow_vfo = false;
    if (sel < 0) sel = nearest_to_vfo();
    else sel += step;
    if (sel < 0) sel = 0;
    if (sel >= view_count) sel = view_count - 1;
    scroll_to_show_row(row_of_spot(sel));
    lv_obj_invalidate(table);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (popup && popup_key(key)) return;
    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            move_selection(-1);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            move_selection(1);
            break;
        case LV_KEY_ENTER:
            tune_to(sel);
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

/* MFK press. The dialog is not editable, so the encoder sends a click and
 * no ENTER key; taps elsewhere on the dialog are ignored. */
static void dialog_click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;
    if (dialog_cluster_alert_press()) return;
    if (popup) use_server(srv_sel);
    else tune_to(sel);
}

/* A tap on a row tunes to it. */
static void table_click_cb(lv_event_t *e) {
    (void)e;
    uint16_t r, c;
    lv_table_get_selected_cell(table, &r, &c);
    if (r >= row_count || rows[r] == ROW_VFO) return;
    sel = rows[r];
    tune_to(sel);
    lv_obj_invalidate(table);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_recolor(l, true);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    now_min    = utc_now_min();
    follow_vfo = true;
    sel        = -1;
    last_vfo   = cparam_i_get(cfg_fg_freq);
    last_band  = vfo_band();
    spots_gen  = 0;
    all_count  = 0;
    view_mode  = 0;

    lv_color_t head_color = TC(text_head);
    head_left             = make_label(dialog.obj, VF(22), head_color);
    lv_obj_set_pos(head_left, 18, 12);
    lv_obj_set_width(head_left, 350);
    head_mid = make_label(dialog.obj, VF(22), head_color);
    lv_obj_set_width(head_mid, 250);
    lv_obj_set_style_text_align(head_mid, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(head_mid, 800 / 2 - 125 + 40, 12);
    head_right = make_label(dialog.obj, VF(22), head_color);
    lv_obj_set_width(head_right, 260);
    lv_obj_set_style_text_align(head_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(head_right, 796 - 18 - 260, 12);
    if (theme_shape.vision) {   /* bigger type: the band and VFO are big in the strip already */
        lv_obj_add_flag(head_mid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(head_left, 400);
        lv_label_set_long_mode(head_left, LV_LABEL_LONG_DOT);
        lv_obj_set_width(head_right, 360);
        lv_obj_set_x(head_right, 796 - 18 - 360);
    }

    table = lv_table_create(dialog.obj);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(table, WIDTH, TABLE_H);
    lv_obj_set_pos(table, 13, TABLE_Y);
    lv_table_set_col_cnt(table, 1);
    lv_table_set_col_width(table, 0, WIDTH - 2);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, TC(panel), LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, TC(dialog_border), LV_PART_MAIN);
    lv_obj_set_style_border_opa(table, 96, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(table, TABLE_H / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, FONT_MAIN, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(table, table_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, table_draw_end_cb, LV_EVENT_DRAW_PART_END, NULL);
    lv_obj_add_event_cb(table, table_click_cb, LV_EVENT_CLICKED, NULL);

    status_left = make_label(dialog.obj, VF(22), lv_color_hex(COLOR_DIM));
    lv_obj_set_pos(status_left, 18, DIALOG_H - STATUS_H - 4);
    lv_obj_set_width(status_left, 440);
    lv_label_set_text(status_left, "MFK: browse - press: tune - VFO: follow");
    status_right = make_label(dialog.obj, VF(22), lv_color_hex(COLOR_DIM));
    lv_obj_set_width(status_right, 320);
    lv_obj_set_style_text_align(status_right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(status_right, 796 - 18 - 320, DIALOG_H - STATUS_H - 4);

    /* The MFK drives the list: the dialog itself takes the keys. */
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, dialog_click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    load_server();

    timer_cb(NULL);
    timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
}

static void destruct_cb(void) {
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    /* The client keeps running: the spots show at once when the app is
     * opened again. */
    table = popup = NULL;
}

static void close_cb(button_data_t *data) {
    (void)data;
    dialog_destruct();
}

static void band_all_cb(button_data_t *data) {
    bool all = !param_i_get(cfg_cluster_all_bands);
    param_i_set(cfg_cluster_all_bands, all);
    buttons_refresh(data);
    follow_vfo = true;
    rebuild();
    update_header();
    msg_update_text_fmt("Cluster: %s", all ? "all bands" : "current band");
}

/* The view shown: all bands, or the VFO's band by name */
static const char *band_label(void) {
    static char label[24];
    int         band = vfo_band();
    snprintf(label, sizeof(label), "Band /\n%s", showing_all() ? "All" : cluster_band_name(band));
    return label;
}

static const char *order_label(void) {
    return by_time() ? "Order /\nTime" : "Order /\nFreq";
}

/* By frequency around the VFO <-> newest first */
static void order_cb(button_data_t *data) {
    param_i_set(cfg_cluster_order, !by_time());
    buttons_refresh(data);
    follow_vfo = true;
    rebuild();
    msg_update_text_fmt("Cluster: %s", by_time() ? "newest first" : "by frequency");
}

static const char *link_label(void) {
    return cluster_config_enabled() ? "Link /\nOn" : "Link /\nOff";
}

/* The cluster login on and off: off, the radio holds no session on any node (the POTA, WWBOTA and HOTA spots
 * keep coming over HTTPS only while it is on) */
static void link_cb(button_data_t *data) {
    bool on = !cluster_config_enabled();

    if (!cluster_enabled_save(on)) {
        msg_update_text_fmt("Cluster: can't save");
        return;
    }
    load_server();
    buttons_refresh(data);
    update_status();
    msg_update_text_fmt("Cluster: %s", on ? "link on" : "link off");
}

static const char *mode_label(void) {
    static const char *const label[] = {"Mode /\nAll", "Mode /\nCW", "Mode /\nSSB", "", "Mode /\nDIG"};
    return label[view_mode];
}

/* All -> CW -> SSB -> DIG -> All */
static void mode_cb(button_data_t *data) {
    view_mode = view_mode ? (view_mode == CLUSTER_FM_DIG ? 0 : view_mode << 1) : CLUSTER_FM_CW;
    buttons_refresh(data);
    if (table) {
        follow_vfo = true;
        rebuild();
        update_header();
    }
}

static void tune_cb(button_data_t *data) {
    (void)data;
    tune_to(sel);
}

/* Server popup: the saved servers, the MFK chooses and a press uses one. */
static void show_servers(void) {
    for (int i = 0; i < srv_n; i++) {
        bool on = i == srv_sel;
        lv_obj_set_style_bg_opa(srv_rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(srv_rows[i], on ? 2 : 0, 0);
        lv_obj_t *name = lv_obj_get_child(srv_rows[i], 0);
        lv_obj_set_style_text_color(name, on ? lv_color_hex(COLOR_NEAR) : TC(text), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(srv_rows[i], 1), on ? lv_color_hex(COLOR_NEAR) : lv_color_hex(COLOR_DIM), 0);
        if (on) lv_obj_scroll_to_view(srv_rows[i], LV_ANIM_OFF);
    }
}

static void use_server(int i) {
    if (i < 0 || i >= srv_n) return;
    if (!cluster_config_save(srv[i].host, srv[i].port)) {
        msg_schedule_text_fmt("Cannot save %s", CLUSTER_CONF);
        return;
    }
    load_server();
    timer_cb(NULL);
    for (int r = 0; r < srv_n; r++)
        lv_obj_add_flag(lv_obj_get_child(srv_rows[r], 2), LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lv_obj_get_child(srv_rows[i], 2), LV_OBJ_FLAG_HIDDEN);
    msg_schedule_text_fmt("Cluster: %s:%d", server, server_port);
}

static void close_popup(void) {
    lv_obj_del(popup);
    popup = NULL;
}

static bool popup_key(uint32_t key) {
    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            if (srv_sel > 0) srv_sel--;
            show_servers();
            return true;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            if (srv_sel < srv_n - 1) srv_sel++;
            show_servers();
            return true;
        case LV_KEY_ENTER:
            use_server(srv_sel);
            return true;
        case LV_KEY_ESC:
            close_popup();
            return true;
        default:
            return false;
    }
}

static void server_row_click_cb(lv_event_t *e) {
    srv_sel = (int)(intptr_t)lv_event_get_user_data(e);
    show_servers();
    use_server(srv_sel);
}

static void open_popup(void) {
    srv_n = cluster_servers_load(srv, CLUSTER_MAX_SERVERS);

    popup = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(popup);
    lv_obj_set_size(popup, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(popup, TC(veil), 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);

    int rows_h = srv_n * POP_ROW_H;
    if (rows_h > POP_MAX_ROWS_H) rows_h = POP_MAX_ROWS_H;
    int box_h = POP_HEAD_H + rows_h + POP_HINT_H;

    lv_obj_t *box = lv_obj_create(popup);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, POP_W, box_h);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_style(box, &popup_style, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_label_create(box);
    lv_obj_set_style_text_font(head, VF(22), 0);
    lv_obj_set_style_text_color(head, TC(text_head), 0);
    lv_label_set_text(head, "CLUSTER SERVERS");
    lv_obj_set_pos(head, 22, 9);
    lv_obj_t *count = lv_label_create(box);
    lv_obj_set_style_text_font(count, VF(22), 0);
    lv_obj_set_style_text_color(count, TC(text_head), 0);
    lv_label_set_text_fmt(count, "%d", srv_n);
    lv_obj_align(count, LV_ALIGN_TOP_RIGHT, -22, 9);

    lv_obj_t *list = lv_obj_create(box);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, POP_W - 4, rows_h);
    lv_obj_set_pos(list, 0, POP_HEAD_H);

    srv_sel = 0;
    for (int i = 0; i < srv_n; i++) {
        bool inuse = strcmp(srv[i].host, server) == 0 && srv[i].port == server_port;
        if (inuse) srv_sel = i;

        lv_obj_t *row = lv_obj_create(list);
        srv_rows[i]   = row;
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, POP_W - 4, POP_ROW_H);
        lv_obj_set_pos(row, 0, i * POP_ROW_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_SEL_BG), 0);
        lv_obj_set_style_border_color(row, TC(sel_border), 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_add_event_cb(row, server_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_font(name, VF(28), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name, 220);
        lv_label_set_text(name, srv[i].name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 22, 0);

        char addr[sizeof(srv[i].host) + 8];
        snprintf(addr, sizeof(addr), "%s:%d", srv[i].host, srv[i].port);
        lv_obj_t *a = lv_label_create(row);
        lv_obj_set_style_text_font(a, VF(20), 0);
        lv_label_set_long_mode(a, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(a, 210);
        lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(a, addr);
        lv_obj_align(a, LV_ALIGN_RIGHT_MID, -22, 0);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_NEW), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -22 - lv_txt_get_width(addr, strlen(addr), VF(20), 0, 0) - 14, 0);
        if (!inuse) lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *hint = lv_label_create(box);
    lv_obj_set_style_text_font(hint, VF(18), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, POP_W - 44);
    lv_label_set_text(hint, "MFK: choose - press: use - Server: close - add servers from the web page");
    lv_obj_set_pos(hint, 22, POP_HEAD_H + rows_h + 8);

    show_servers();
}

static void server_cb(button_data_t *data) {
    (void)data;
    if (popup) close_popup();
    else open_popup();
}

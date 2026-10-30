/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cluster_column.h"

#include "cluster/client.h"
#include "cfg/cfg_api.h"
#include "dialog_cluster.h"
#include "styles.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fonts/font_select.h"

/* VISION: the sizes of the other themes here (an overlay on VISION's shorter waterfall) */
#define NF(n) (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS) : &sony_##n)

#define COL_W   244
#define TITLE_H 30
#define ROW_H   28
#define POOL    16
#define FREQ_W  65      /* 5 digits of sony_22 (12.9 px each), e.g. 14074 */

typedef struct {
    lv_obj_t *freq, *call, *mode, *age;
} row_t;

static lv_obj_t      *box, *hl, *title;
static row_t          rows[POOL];
static cluster_spot_t spots[CLUSTER_MAX_SPOTS];
static int            spots_n;
static uint32_t       spots_gen;
static uint16_t       last_min = 0xFFFF;
static uint32_t       last_fgen;
static int32_t        last_vfo = -1;
static int32_t        last_order = -1, last_all = -1;
static int32_t        vfo;
static uint16_t       sort_now;
static cluster_spot_t band[CLUSTER_MAX_SPOTS];  /* the listed spots, by frequency or newest first */
static int            band_n;
static bool           active;                   /* MFK item "Cluster" */
static bool           has_sel;
static bool           hidden;                   /* cluster_column_hide(): a game over the waterfall */
static cluster_spot_t sel;                      /* highlighted spot, kept by call + freq */

/* Outdoor themes: the whole column in the text colour, the colours fade in the sun */
static uint32_t ink(uint32_t c) {
    return theme_shape.mono_cluster ? theme_hex(TC_text) : c;
}

static uint32_t age_color(uint16_t age) {
    if (age < 2)  return theme_hex(TC_new);
    if (age < 10) return theme_hex(TC_age_mid);
    if (age < 30) return theme_hex(TC_age_old);
    return theme_hex(TC_text_mute);
}

/* Mode the spot tunes to (as the Cluster app does) */
static void set_mode(lv_obj_t *l, const cluster_spot_t *s) {
    static const char     *name[]  = { "CW", "LSB", "USB", "DIG", "AM", "FM" };
    static const theme_color_t color[] = { TC_warn, TC_vfo, TC_vfo, TC_digi, TC_text_value, TC_text_value };
    cluster_mode_t         m = cluster_guess_mode(s);

    lv_label_set_text(l, name[m]);
    lv_obj_set_style_text_color(l, lv_color_hex(ink(theme_hex(color[m]))), 0);
}

static int by_freq(const void *a, const void *b) {
    return ((const cluster_spot_t *)a)->freq_hz - ((const cluster_spot_t *)b)->freq_hz;
}

static int by_age(const void *a, const void *b) {
    return cluster_age_min(((const cluster_spot_t *)a)->utc_min, sort_now) -
           cluster_age_min(((const cluster_spot_t *)b)->utc_min, sort_now);
}

static void show_row(row_t *r, bool show) {
    void (*f)(lv_obj_t *, lv_obj_flag_t) = show ? lv_obj_clear_flag : lv_obj_add_flag;

    f(r->freq, LV_OBJ_FLAG_HIDDEN);
    f(r->call, LV_OBJ_FLAG_HIDDEN);
    f(r->mode, LV_OBJ_FLAG_HIDDEN);
    f(r->age, LV_OBJ_FLAG_HIDDEN);
}

/* Alert rows are magenta (not under the highlight, which shows through) */
static void set_alert_bg(row_t *r, bool on) {
    lv_obj_t *l[] = {r->freq, r->call, r->mode, r->age};

    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(l[i], on ? TC(alert_bg) : TC(vfo_bg), 0);
        lv_obj_set_style_bg_opa(l[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    if (theme_shape.mono) {     /* 1bit: alert_bg is the paper; the call framed instead */
        lv_obj_set_style_border_width(r->call, on ? 1 : 0, 0);
        lv_obj_set_style_border_color(r->call, TC(text), 0);
    }
}

static void show_spot(row_t *r, const cluster_spot_t *s, bool highlighted, const cluster_filter_t *flt, uint16_t now) {
    uint16_t age   = cluster_age_min(s->utc_min, now);
    uint32_t c     = age_color(age);
    bool     alert = cluster_alert_match(flt, s) >= 0;
    bool     can   = cfg_is_valid_hw_freq(s->freq_hz);     /* greyed: no transverter for it (All bands) */
    char     buf[16];

    snprintf(buf, sizeof buf, "%d", (int)(s->freq_hz / 1000));
    lv_label_set_text(r->freq, buf);
    /* 144300: six digits, a size smaller to fit, on the line of the call and the mode */
    lv_obj_set_style_text_font(r->freq, s->freq_hz >= 100000000 ? NF(18) : NF(22), 0);
    lv_obj_set_style_pad_top(r->freq, s->freq_hz >= 100000000 ? 3 : 0, 0);
    lv_obj_set_width(r->freq, FREQ_W);
    lv_obj_set_style_text_align(r->freq, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(r->freq, lv_color_hex(ink(theme_hex(can ? TC_text_value : TC_text_mute))), 0);
    set_alert_bg(r, alert && !highlighted);
    lv_label_set_text(r->call, s->dx);
    set_mode(r->mode, s);
    snprintf(buf, sizeof buf, "%um", age);
    lv_label_set_text(r->age, buf);
    lv_obj_set_style_text_color(r->call,
                                lv_color_hex(ink(alert ? theme_hex(TC_alert_text) : can ? c : theme_hex(TC_text_mute))), 0);
    lv_obj_set_style_text_color(r->age, lv_color_hex(ink(c)), 0);
    show_row(r, true);
}

/* By frequency: the spots with the VFO marker row among them, kept in the
 * middle of the box: tuning scrolls the names past it. Newest first: the
 * list from the top, no marker. */
static void redraw(void) {
    int       n = 0, m = 0, si = -1;
    int       cb = cluster_band_of(vfo);
    bool      all = param_i_get(cfg_cluster_all_bands);
    bool      by_time = param_i_get(cfg_cluster_order);
    const cluster_filter_t *flt = cluster_filter();
    time_t    t  = time(NULL);
    struct tm tm;
    uint16_t  now;

    if (!box) {
        return;
    }
    if (!param_i_get(cfg_cluster_column) || hidden) {
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);
    gmtime_r(&t, &tm);
    now = tm.tm_hour * 60 + tm.tm_min;

    last_fgen  = flt->gen;
    last_order = by_time;
    last_all   = all;
    lv_label_set_text_fmt(title, "Cluster %s - %s", all ? "All" : (cb >= 0 ? cluster_band_name(cb) : "--"),
                          by_time ? "Time" : "Freq");
    if (all || cb >= 0) {
        for (int i = 0; i < spots_n; i++) {
            if ((all || cluster_band_of(spots[i].freq_hz) == cb) && cluster_filter_pass(flt, &spots[i], now)) {
                band[n++] = spots[i];
            }
        }
        sort_now = now;
        qsort(band, n, sizeof(band[0]), by_time ? by_age : by_freq);
    }
    band_n = n;
    while (!by_time && m < n && band[m].freq_hz < vfo) {
        m++;    /* the marker goes before band[m] */
    }
    if (active && n > 0) {
        for (int i = 0; i < n && has_sel; i++) {
            if (band[i].freq_hz == sel.freq_hz && !strcmp(band[i].dx, sel.dx)) {
                si = i;
            }
        }
        if (si < 0) {   /* none yet, or gone: the newest, or the spot nearest above the VFO */
            si  = by_time ? 0 : (m < n ? m : n - 1);
            sel = band[si];
            has_sel = true;
        }
    }
    int fit = (lv_obj_get_height(box) - TITLE_H - 8) / ROW_H;

    if (fit > POOL) {
        fit = POOL;
    } else if (fit < 0) {
        fit = 0;    /* not laid out yet */
    }
    if (si >= 0) {
        lv_obj_clear_flag(hl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(hl, LV_OBJ_FLAG_HIDDEN);
    }
    if (by_time) {
        int first = si - fit / 2;   /* the highlight in the middle once the list is long enough */

        if (first > n - fit) {
            first = n - fit;
        }
        if (first < 0) {
            first = 0;
        }
        for (int k = 0; k < fit; k++) {
            int idx = first + k;

            if (idx >= n) {
                show_row(&rows[k], false);
                continue;
            }
            if (idx == si) {
                lv_obj_set_y(hl, TITLE_H + k * ROW_H);
            }
            show_spot(&rows[k], &band[idx], idx == si, flt, now);
        }
        for (int k = fit; k < POOL; k++) {
            show_row(&rows[k], false);
        }
        return;
    }
    int base = si < 0 ? m : (si < m ? si : si + 1);  /* active: the highlight stays in the middle */

    for (int k = 0; k < fit; k++) {
        int   idx = base - fit / 2 + k;     /* < m: spots below the marker, m: the marker, > m: spots (idx - 1) */
        row_t *r  = &rows[k];

        if (idx == base && si >= 0) {
            lv_obj_set_y(hl, TITLE_H + k * ROW_H);
        }
        if (idx < 0 || idx > n) {
            show_row(r, false);
        } else if (idx == m) {
            char buf[32];

            snprintf(buf, sizeof buf, "%d.%03d.%02d", vfo / 1000000, (vfo / 1000) % 1000, (vfo % 1000) / 10);
            lv_label_set_text(r->freq, buf);
            lv_obj_set_style_text_font(r->freq, NF(22), 0);     /* the row may have had a 2 m spot's */
            lv_obj_set_style_pad_top(r->freq, 0, 0);
            lv_obj_set_width(r->freq, COL_W - 12);
            lv_obj_set_style_text_align(r->freq, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(r->freq, lv_color_hex(ink(theme_hex(TC_vfo))), 0);
            set_alert_bg(r, false);
            lv_obj_set_style_bg_opa(r->freq, LV_OPA_COVER, 0);
            show_row(r, true);
            lv_obj_add_flag(r->call, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r->mode, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r->age, LV_OBJ_FLAG_HIDDEN);
        } else {
            show_spot(r, &band[idx < m ? idx : idx - 1], idx == base && si >= 0, flt, now);
        }
    }
    for (int k = fit; k < POOL; k++) {
        show_row(&rows[k], false);
    }
}

/* 4 Hz, redraws only when the spots, the minute, the VFO or the view changed. Not at each
 * VFO step: its 64 labels laid out again at every step slowed tuning down */
static void timer_cb(lv_timer_t *t) {
    int      n   = cluster_client_snapshot(spots, CLUSTER_MAX_SPOTS, &spots_gen);
    uint16_t min = (time(NULL) / 60) % 1440;

    vfo = cparam_i_get(cfg_fg_freq);
    if (n >= 0) {
        spots_n = n;
    }
    /* Newest first: the VFO only matters when it changes band */
    bool moved = vfo != last_vfo && (!param_i_get(cfg_cluster_order) || cluster_band_of(vfo) != cluster_band_of(last_vfo));

    if (n >= 0 || min != last_min || moved || cluster_filter()->gen != last_fgen ||
        param_i_get(cfg_cluster_order) != last_order || param_i_get(cfg_cluster_all_bands) != last_all) {
        last_min = min;
        last_vfo = vfo;
        redraw();
    }
}

void cluster_column_hide(bool hide) {
    hidden = hide;
    redraw();
}

void cluster_column_set_active(bool on) {
    active = on;
    redraw();
}

const char *cluster_column_move(int dir) {
    int i = -1;

    for (int k = 0; k < band_n && has_sel; k++) {
        if (band[k].freq_hz == sel.freq_hz && !strcmp(band[k].dx, sel.dx)) {
            i = k;
        }
    }
    if (band_n > 0 && i >= 0) {
        i += dir;
        i = i < 0 ? 0 : (i >= band_n ? band_n - 1 : i);
        sel = band[i];
        redraw();
    }
    return has_sel && band_n > 0 ? sel.dx : "";
}

void cluster_column_tune(void) {
    if (active && has_sel && band_n > 0) {
        dialog_cluster_tune_spot(&sel);
    }
}

void cluster_column_layout(lv_coord_t y) {
    if (box) {
        lv_obj_set_y(box, y);
        lv_obj_set_height(box, 480 - BUTTONS_H - y);
        redraw();
    }
}

void cluster_column_refresh(void) {
    vfo      = cparam_i_get(cfg_fg_freq);
    last_vfo = vfo;
    redraw();
}

bool cluster_column_area(lv_area_t *a) {
    if (!box || lv_obj_has_flag(box, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    a->x1 = lv_obj_get_x(box);
    a->y1 = lv_obj_get_y(box);
    a->x2 = a->x1 + lv_obj_get_width(box) - 1;
    a->y2 = a->y1 + lv_obj_get_height(box) - 1;
    return true;
}

static lv_obj_t *cell(lv_obj_t *p, lv_coord_t x, lv_coord_t w, lv_coord_t y, lv_text_align_t al, uint32_t c,
                      const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(p);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_obj_set_style_text_align(l, al, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_bg_color(l, TC(vfo_bg), 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

void cluster_column_init(lv_obj_t *parent, lv_coord_t y) {
    bool again = box != NULL;   /* a theme change: the timer stays, the new rows are filled */

    box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 800 - COL_W, y);
    lv_obj_set_size(box, COL_W, 480 - BUTTONS_H - y);
    lv_obj_set_style_radius(box, theme_shape.radius_popup, 0);
    lv_obj_set_style_bg_color(box, TC(side), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_60, 0);
    lv_obj_set_style_border_color(box, TC(panel_border), 0);
    lv_obj_set_style_border_width(box, theme_shape.border, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    hl = lv_obj_create(box);
    lv_obj_remove_style_all(hl);
    lv_obj_set_pos(hl, 2, TITLE_H);
    lv_obj_set_size(hl, COL_W - 6, ROW_H);
    lv_obj_set_style_radius(hl, 4, 0);
    lv_obj_set_style_bg_color(hl, TC(sel_bg), 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hl, TC(sel_text), 0);
    lv_obj_set_style_border_width(hl, 2, 0);
    lv_obj_add_flag(hl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(hl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    title = cell(box, 0, COL_W - 2, 2, LV_TEXT_ALIGN_CENTER, ink(theme_hex(TC_text_head)), NF(22));
    lv_label_set_text(title, "DX Cluster");
    lv_obj_clear_flag(title, LV_OBJ_FLAG_HIDDEN);

    /* Call, mode and age in smaller fonts with a gap between the fields: long
     * calls no longer run into the frequency and the mode */
    for (int i = 0; i < POOL; i++) {
        lv_coord_t ry = TITLE_H + i * ROW_H;

        rows[i].freq = cell(box, 4, FREQ_W, ry, LV_TEXT_ALIGN_LEFT, theme_hex(TC_text_value), NF(22));
        rows[i].call = cell(box, 72, 99, ry + 2, LV_TEXT_ALIGN_LEFT, theme_hex(TC_text), NF(20));
        lv_label_set_long_mode(rows[i].call, LV_LABEL_LONG_DOT);   /* a long call ends in dots, clear of the mode */
        rows[i].mode = cell(box, 173, 32, ry + 3, LV_TEXT_ALIGN_LEFT, theme_hex(TC_text), NF(18));
        rows[i].age  = cell(box, 207, 32, ry + 3, LV_TEXT_ALIGN_RIGHT, theme_hex(TC_text), NF(18));
    }
    if (!again) lv_timer_create(timer_cb, 250, NULL);
    lv_obj_update_layout(box);  /* its height, for the rows redraw() fits */
    last_min = 0xFFFF;          /* drawn now, not at the next spot */
    timer_cb(NULL);
}

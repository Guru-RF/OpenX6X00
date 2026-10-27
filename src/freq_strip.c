/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Frequency strip: band on the left, VFO frequency in the middle, then the
 *  locator and the mode on the right; over the locator the TX audio offset of
 *  FT8/FT4/WSPR (the big digits stay on the dial, as WSJT-X). Always shown; the app dialogs start
 *  below it. A line under the digit the tuning step changes, and NR NB DNF (RIT XIT when set)
 *  over the locator, as upstream v1.0.0's step marker and indicators row.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The character of text ("%i.%03i.%03i", the first frequency of a split) that a tuning step
 * changes, -1 if none; *big when the step is 2-9 of that digit (a full mark), 1 a half one.
 * The digit and mark size as Dyuldin's upstream freq_info.cpp fg_freq_post_draw_event_cb() */
int freq_strip_step_char(const char *text, int32_t step, bool *big) {
    int digit = 0;   /* 0 the Hz units */

    while (step >= 10) {
        step /= 10;
        digit++;
    }
    *big = step > 1;

    int pos = (int)strcspn(text, " ") - 1 - digit - digit / 3;   /* a dot every 3 digits */

    return pos >= 0 && text[pos] >= '0' && text[pos] <= '9' ? pos : -1;
}

#ifndef FREQ_STRIP_STEP_ONLY   /* tests/test_freq_strip.cpp: the digit alone */

#include "freq_strip.h"

#include "broadcast_db.h"
#include "cfg/cfg_api.h"
#include "cluster/spots.h"
#include "info.h"
#include "main_screen.h"
#include "params/params.h"
#include "station.h"
#include "styles.h"
#include "theme.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#define QTH_POLL_MS 3000
#define BC_TOLERANCE_HZ 2000    /* as the SWL panel of the GENE key */

static char      app[16];
static lv_obj_t *strip, *band_label, *freq_label, *mode_label, *qth_dot, *qth_label, *tx_label;
static lv_obj_t *nr_label, *nb_label, *dnf_label, *rit_label, *xit_label;
static lv_obj_t *freq_small, *other_label, *vfo_badge;   /* deck: the 10 Hz digits, the other VFO, A or B */
static lv_obj_t *mid, *col;
static bool      bc_tagged;         /* VISION: the band label is the broadcast tag */

static void vision_fit(void);
static char      bc[260];           /* the broadcast on air on the VFO, "" none */
static int32_t   bc_freq = -1;
static time_t    bc_minute = -1;
static char      note[64];
static int32_t   note_freq;
static int32_t   tx_offset = -1;

/* The step marker, under the digit (in the label's own box, so it is redrawn with the text) */
static void step_mark_draw_cb(lv_event_t *e) {
    lv_obj_t   *label = lv_event_get_target(e);
    const char *text = lv_label_get_text(freq_label);
    char        full[48];
    bool        big;

    if (theme_shape.deck) {   /* "14.100" and ".00": the digit is in one of them, the units are not shown */
        snprintf(full, sizeof(full), "%s%s0", text, lv_label_get_text(freq_small));
        text = full;
    }

    int pos = freq_strip_step_char(text, param_i_get(cfg_mode_freq_step), &big);

    if (theme_shape.deck && label == freq_small) pos -= (int)strlen(lv_label_get_text(freq_label));
    if (pos < 0 || pos >= (int)strlen(lv_label_get_text(label)) || main_screen_freq_locked()) return;

    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    lv_area_t        a;
    lv_point_t       p1, p2;

    lv_obj_get_content_coords(label, &a);
    lv_label_get_letter_pos(label, pos, &p1);
    lv_label_get_letter_pos(label, pos + 1, &p2);
    p1.x += a.x1;
    p2.x += a.x1;
    p1.y = p2.y = a.y1 + p1.y + font->line_height - font->base_line + 2;
    if (!big) {   /* a step of 1: half as wide */
        lv_coord_t w = (p2.x - p1.x) / 2;

        p1.x += w / 2;
        p2.x = p1.x + w;
    }

    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = theme_shape.deck ? TC(info) : TC(ok);
    dsc.width = theme_shape.deck ? 3 : 2;
    lv_draw_line(lv_event_get_draw_ctx(e), &dsc, &p1, &p2);
}

static void step_change_cb(Subject *subj, void *user_data) {
    lv_obj_invalidate(freq_label);
}

/* NR NB DNF lit when on, dim when off; RIT XIT only when set. Lit as PRE / ATT in the info box */
static void ind_set(lv_obj_t *l, bool on) {
    lv_obj_set_style_text_color(l, on ? TC(focus_text) : TC(text_mute), 0);
    lv_obj_set_style_bg_color(l, TC(focus), 0);
    lv_obj_set_style_bg_opa(l, on ? (theme_shape.focus_solid ? LV_OPA_COVER : LV_OPA_50) : LV_OPA_0, 0);
}

static void ind_update_cb(Subject *subj, void *user_data) {
    ind_set(nr_label, param_i_get(cfg_nr));
    ind_set(nb_label, param_i_get(cfg_nb));
    ind_set(dnf_label, param_i_get(cfg_dnf));
    ind_set(rit_label, true);
    ind_set(xit_label, true);
    if (param_i_get(cfg_rit)) lv_obj_clear_flag(rit_label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(rit_label, LV_OBJ_FLAG_HIDDEN);
    if (param_i_get(cfg_xit)) lv_obj_clear_flag(xit_label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(xit_label, LV_OBJ_FLAG_HIDDEN);
    vision_fit();
}

static lv_obj_t *ind_create(lv_obj_t *parent, const char *text) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, theme_shape.vision ? &sony_24 : &sony_18, 0);
    lv_obj_set_style_pad_hor(l, 3, 0);
    lv_obj_set_style_radius(l, 3, 0);
    lv_label_set_text(l, text);
    return l;
}

/* The station nearest the VFO among those on air (EiBi), looked up only when the frequency or
 * the minute changed: "Name (language)" */
static void bc_lookup(int32_t f) {
    broadcast_match_t m[BROADCAST_MAX_MATCHES];
    time_t            minute = time(NULL) / 60;
    size_t            n = 0, best = 0;

    if (f == bc_freq && minute == bc_minute) return;
    bc_freq = f;
    bc_minute = minute;
    if (param_i_get(cfg_broadcast_name)) n = broadcast_db_find_matches(f, BC_TOLERANCE_HZ, m, BROADCAST_MAX_MATCHES);
    for (size_t i = 1; i < n; i++)
        if (fabs(m[i].freq_khz * 1000.0 - f) < fabs(m[best].freq_khz * 1000.0 - f)) best = i;
    if (!n) bc[0] = 0;
    else if (m[best].language[0]) snprintf(bc, sizeof(bc), "%s (%s)", m[best].station, m[best].language);
    else snprintf(bc, sizeof(bc), "%s", m[best].station);
}

/* A broadcast station in a tag, "BC" in front, so it is not read as a band or a memory */
static void bc_tag(bool on) {
    lv_obj_set_style_border_width(band_label, on ? 1 : 0, 0);
    lv_obj_set_style_border_color(band_label, TC(info), 0);
    lv_obj_set_style_radius(band_label, 4, 0);
    lv_obj_set_style_pad_hor(band_label, on ? 6 : 0, 0);
}

/* The tag's text, the name cut with dots to fit w px (the label's own dots broke on the colour
 * tag: an empty box in VISION) */
static void bc_fit(lv_coord_t w) {
    const lv_font_t *font = lv_obj_get_style_text_font(band_label, 0);
    lv_coord_t       ls = lv_obj_get_style_text_letter_space(band_label, 0);
    size_t           len = strlen(bc), n = len;
    char             s[sizeof(bc) + 8];

    w -= 2 * 6 + 2;   /* the tag's padding and border */
    for (;;) {
        snprintf(s, sizeof(s), "BC  %.*s%s", (int)n, bc, n < len ? "..." : "");
        if (!n || lv_txt_get_width(s, strlen(s), font, ls, LV_TEXT_FLAG_NONE) <= w) break;
        do n--; while (n && (bc[n] & 0xc0) == 0x80);   /* whole UTF-8 letters */
    }
    lv_label_set_text_fmt(band_label, "#%06x BC#  %s", theme_hex(TC_info), s + 4);
}

/* VISION's big type: the frequency and the mode move left of the column on the right when it
 * grows (NR NB DNF RIT XIT, the TX offset of the apps), a long note (NAVTEX, a memory) or
 * station ends in dots before them */
static void vision_fit(void) {
    if (!theme_shape.vision) return;
    lv_obj_align(mid, LV_ALIGN_CENTER, 0, 0);
    lv_obj_update_layout(strip);

    lv_coord_t over = lv_obj_get_x(mid) + lv_obj_get_width(mid) + 12 - lv_obj_get_x(col);

    if (over > 0) {
        lv_obj_align(mid, LV_ALIGN_CENTER, -over, 0);
        lv_obj_update_layout(strip);
    }

    lv_coord_t room = lv_obj_get_x(mid) - lv_obj_get_x(band_label) - 12;

    if (bc_tagged) {
        bc_fit(room);
        return;
    }
    lv_label_set_long_mode(band_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(band_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_update_layout(strip);
    if (!app[0] && lv_obj_get_width(band_label) > room) {   /* the app's two lines are short */
        lv_label_set_long_mode(band_label, LV_LABEL_LONG_DOT);
        lv_obj_set_size(band_label, room, lv_font_get_line_height(lv_obj_get_style_text_font(band_label, 0)));
    }
}

static void update_cb(Subject *subj, void *user_data) {
    (void)subj;
    (void)user_data;
    uint16_t mhz, khz, hz;
    int32_t  f = cparam_i_get(cfg_fg_freq);

    bc_lookup(f);

    split_freq(f, &mhz, &khz, &hz);
    if (theme_shape.deck) {     /* the other VFO at the top right, the current one in the badge */
        bool        b = param_i_get(cfg_band_current_vfo) == RADIO_VFO_B;
        uint16_t    mhz2, khz2, hz2;
        /* a TX shift after it ("-600k", "REV"); in split, in the error colour where the radio cannot transmit */
        const char *shift = info_params_tx_shift_label();
        bool        blocked = info_tx_blocked();
        char        tail[32] = "";

        if (shift) snprintf(tail, sizeof(tail), "  #%06x %s#", theme_hex(blocked ? TC_err : TC_info), shift);
        split_freq(cparam_i_get(cfg_bg_freq), &mhz2, &khz2, &hz2);
        lv_label_set_text_fmt(freq_label, "%i.%03i", mhz, khz);
        lv_label_set_text_fmt(freq_small, ".%02i", hz / 10);
        lv_label_set_text_fmt(other_label, "#%06x %s#  #%06x %i.%03i.%02i#%s", theme_hex(TC_text), b ? "A" : "B",
                              theme_hex(blocked ? TC_err : TC_text_dim), mhz2, khz2, hz2 / 10, tail);
        lv_label_set_text(vfo_badge, b ? "B" : "A");
        lv_label_set_text_fmt(band_label, "#%06x %s#", theme_hex(TC_accent), app);   /* the open app only */
    } else if (param_i_get(cfg_band_split)) {
        uint16_t mhz2, khz2, hz2;
        split_freq(cparam_i_get(cfg_bg_freq), &mhz2, &khz2, &hz2);
        if (theme_shape.mono) lv_obj_set_style_text_font(freq_label, &sony_36, 0);   /* both fit, 23 cells */
        lv_label_set_text_fmt(freq_label, "%i.%03i.%03i / %i.%03i.%03i", mhz, khz, hz, mhz2, khz2, hz2);
    } else {
        extern const lv_font_t pixel_freq_40;

        if (theme_shape.mono) lv_obj_set_style_text_font(freq_label, &pixel_freq_40, 0);
        lv_label_set_text_fmt(freq_label, "%i.%03i.%03i", mhz, khz, hz);
    }
    /* An open app: the band (a broadcast has nothing to do with the digital apps) */
    const char *band = note[0] && f == note_freq ? note : bc[0] && !app[0] ? bc : cluster_band_name(cluster_band_of(f));

    /* The open app in front of the band, so it's clear where the keys go */
    if (theme_shape.deck) {
    } else if (app[0] && theme_shape.vision) {   /* big type: on two lines, or it ran into the frequency */
        lv_obj_set_style_text_font(band_label, &sony_24, 0);
        lv_label_set_text_fmt(band_label, "#%06x %s#\n%s", theme_hex(TC_accent), app, band);
    } else if (app[0]) {
        lv_label_set_text_fmt(band_label, "#%06x %s# %s", theme_hex(TC_accent), app, band);
    } else {
        if (theme_shape.vision) lv_obj_set_style_text_font(band_label, &sony_36, 0);
        lv_label_set_text(band_label, band);
    }
    /* A station: in its tag, on one line up to the frequency (deck: 300 px, before the other
     * VFO), cut with dots */
    if (theme_shape.deck ? bc[0] && !app[0] : band == bc) {
        bc_tag(true);
        lv_obj_set_size(band_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_update_layout(strip);
        bc_fit(theme_shape.deck ? 300 : lv_obj_get_x(mid) - lv_obj_get_x(band_label) - 12);
    } else {
        bc_tag(false);
        lv_obj_set_size(band_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    }
    lv_label_set_text(mode_label, info_params_mode_label_get());

    if (tx_offset >= 0) {
        lv_label_set_text_fmt(tx_label, "TX %i Hz", (int)tx_offset);
        lv_obj_clear_flag(tx_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(tx_label, LV_OBJ_FLAG_HIDDEN);
    }
    bc_tagged = band == bc;
    vision_fit();
}

/* Settings: the name on or off now */
static void bc_setting_cb(Subject *subj, void *user_data) {
    bc_freq = -1;
    update_cb(subj, user_data);
}

/* The GPS locator (green dot) while there is a fix, else the saved QTH */
static void qth_update_cb(lv_timer_t *t) {
    (void)t;
    const char *qth = station_qth();

    lv_label_set_text(qth_label, qth);
    if (time(NULL) / 60 != bc_minute) update_cb(NULL, NULL);   /* a station starts or stops */
    if (qth != params.qth.x) {
        lv_obj_clear_flag(qth_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(qth_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, lv_align_t align, lv_coord_t x, const lv_font_t *font,
                            uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, align, x, 0);
    return l;
}

lv_obj_t *freq_strip_init(lv_obj_t *parent) {
    bool again = strip != NULL;   /* a theme change: the timer stays */

    strip = lv_obj_create(parent);
    lv_obj_remove_style_all(strip);
    lv_obj_add_style(strip, &freq_strip_style, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    bool v = theme_shape.vision;

    band_label = make_label(strip, LV_ALIGN_LEFT_MID, 14, v ? &sony_36 : &sony_24, theme_hex(TC_text_head));
    lv_label_set_recolor(band_label, true);
    /* Centre: frequency, then the mode */
    mid = lv_obj_create(strip);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    /* VISION: the mode box centred on the big digits (on their baseline it stood too low) */
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, v ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_END,
                          v ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(mid, 14, 0);
    lv_obj_align(mid, LV_ALIGN_CENTER, 0, 0);
    extern const lv_font_t pixel_freq_40;   /* 1bit: bold pixels doubled, as an LCD's big digits */

    freq_label = make_label(mid, LV_ALIGN_DEFAULT, 0, theme_shape.mono ? &pixel_freq_40 : v ? &sony_60 : &sony_36,
                            theme_hex(TC_text));
    mode_label = make_label(mid, LV_ALIGN_DEFAULT, 0, v ? &sony_36 : &sony_24, theme_hex(TC_mode_text));
    lv_obj_set_style_pad_bottom(mode_label, 3, 0);
    if (theme_shape.mode_box) {     /* in a box, as an indicator */
        lv_obj_set_style_bg_color(mode_label, TC(mode_bg), 0);
        lv_obj_set_style_bg_opa(mode_label, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(mode_label, TC(mode_border), 0);
        lv_obj_set_style_border_width(mode_label, 1, 0);
        lv_obj_set_style_radius(mode_label, 3, 0);
        lv_obj_set_style_pad_hor(mode_label, 10, 0);
        lv_obj_set_style_pad_top(mode_label, 2, 0);
        lv_obj_set_style_pad_bottom(mode_label, 6, 0);   /* the box sits on the baseline, as the text did */
        if (v) {
            lv_obj_set_style_radius(mode_label, 8, 0);
            lv_obj_set_style_pad_bottom(mode_label, 4, 0);
            lv_obj_set_style_pad_top(mode_label, 4, 0);
        }
    }

    /* Right: the TX offset (digital apps) over [dot] locator */
    col = lv_obj_create(strip);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(col, LV_ALIGN_RIGHT_MID, -14, 0);

    /* Over the locator: NR NB DNF RIT XIT, then the TX offset */
    lv_obj_t *top = lv_obj_create(col);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 3, 0);
    nr_label = ind_create(top, "NR");
    nb_label = ind_create(top, "NB");
    dnf_label = ind_create(top, "DNF");
    rit_label = ind_create(top, "RIT");
    xit_label = ind_create(top, "XIT");
    subject_subscribe_delayed((Subject *)cfg_nr, ind_update_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_nb, ind_update_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_dnf, ind_update_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_rit, ind_update_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_xit, ind_update_cb, NULL);
    ind_update_cb(NULL, NULL);

    tx_label = make_label(top, LV_ALIGN_DEFAULT, 0, v ? &sony_24 : &sony_20, theme_hex(TC_accent));
    lv_obj_add_flag(tx_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *right = lv_obj_create(col);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 6, 0);

    qth_dot = lv_obj_create(right);
    lv_obj_remove_style_all(qth_dot);
    lv_obj_set_size(qth_dot, 10, 10);
    lv_obj_set_style_radius(qth_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(qth_dot, TC(ok), 0);
    lv_obj_set_style_bg_opa(qth_dot, LV_OPA_COVER, 0);
    qth_label = make_label(right, LV_ALIGN_DEFAULT, 0, v ? &sony_32 : &sony_28, theme_hex(TC_text));

    /* Deck: the meter on the left of the panel, the frequency big with the 10 Hz digits smaller, the
     * other VFO over the right column, the current one in a badge under it; the mode and NR NB DNF
     * RIT XIT are in the info rows, the locator is not shown */
    if (theme_shape.deck) {
        extern const lv_font_t rcond_freq_88, rcond_freq_54;

        lv_obj_set_style_text_font(freq_label, &rcond_freq_88, 0);
        freq_small = make_label(mid, LV_ALIGN_DEFAULT, 0, &rcond_freq_54, theme_hex(TC_text));
        lv_obj_set_style_pad_column(mid, 0, 0);
        lv_obj_set_style_pad_bottom(freq_small, 3, 0);
        lv_obj_align(mid, LV_ALIGN_CENTER, 100, 10);
        lv_obj_add_flag(mode_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(band_label, &sony_20, 0);
        lv_obj_align(band_label, LV_ALIGN_TOP_LEFT, 310, 4);
        lv_obj_t *hide[] = {nr_label, nb_label, dnf_label, rit_label, xit_label, right};
        for (int i = 0; i < 6; i++) lv_obj_add_flag(hide[i], LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(col, LV_ALIGN_TOP_RIGHT, -10, 6);
        other_label = make_label(col, LV_ALIGN_DEFAULT, 0, &sony_24, theme_hex(TC_text_dim));
        lv_label_set_recolor(other_label, true);
        lv_obj_move_to_index(other_label, 0);
        vfo_badge = make_label(strip, LV_ALIGN_BOTTOM_RIGHT, -12, &sony_24, theme_hex(TC_bg));
        lv_obj_align(vfo_badge, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
        lv_obj_set_style_bg_color(vfo_badge, TC(info), 0);
        lv_obj_set_style_bg_opa(vfo_badge, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(vfo_badge, 5, 0);
        lv_obj_add_event_cb(freq_small, step_mark_draw_cb, LV_EVENT_DRAW_POST, NULL);
        subject_subscribe_delayed((Subject *)cfg_band_current_vfo, update_cb, NULL);
    }

    subject_subscribe_delayed((Subject *)cfg_fg_freq, update_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_bg_freq, update_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_band_split, update_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_band_tx_shift, update_cb, NULL);   /* deck: after the other VFO */
    subject_subscribe_delayed((Subject *)cfg_cur_mode, update_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_broadcast_name, bc_setting_cb, NULL);
    subject_subscribe_delayed((Subject *)cfg_mode_freq_step, step_change_cb, NULL);
    lv_obj_add_event_cb(freq_label, step_mark_draw_cb, LV_EVENT_DRAW_POST, NULL);
    update_cb(NULL, NULL);
    qth_update_cb(NULL);
    if (!again) lv_timer_create(qth_update_cb, QTH_POLL_MS, NULL);
    return strip;
}

void freq_strip_set_label(const char *text, int32_t freq_hz) {
    snprintf(note, sizeof(note), "%s", text);
    note_freq = freq_hz;
    if (strip) update_cb(NULL, NULL);
}

void freq_strip_set_app(const char *name) {
    snprintf(app, sizeof(app), "%s", name ? name : "");
    if (strip) update_cb(NULL, NULL);
}

void freq_strip_set_dialog(bool open) {
    extern const lv_font_t rcond_freq_88, rcond_freq_54;

    if (!strip || !theme_shape.deck) return;
    lv_obj_set_style_text_font(freq_label, open ? &rcond_freq_54 : &rcond_freq_88, 0);
    lv_obj_set_style_text_font(freq_small, open ? &sony_32 : &rcond_freq_54, 0);
    lv_obj_align(lv_obj_get_parent(freq_label), LV_ALIGN_CENTER, 100, open ? -14 : 10);
    if (open) lv_obj_add_flag(vfo_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(vfo_badge, LV_OBJ_FLAG_HIDDEN);
}

void freq_strip_set_offset(int32_t offset_hz) {
    tx_offset = offset_hz;
    if (strip) update_cb(NULL, NULL);
}

#endif

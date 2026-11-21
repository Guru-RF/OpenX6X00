/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the CW Tuner's bars and the decoder's speed in the frequency strip (#308)
 *
 *  Settings > Display > MAIN SCREEN > CW tune and WPM: Frequency strip puts them, in CW, in the free space
 *  between the band and the frequency. That space is as wide as the fonts, the band, the frequency (a split's two)
 *  and LOCK leave it, so the strip (freq_strip.c) measures it and takes the first of these that fits:
 *  - a row: the speed's number over "WPM", then the bars at their size on the spectrum (one every 9 px);
 *  - the same row with the bars narrower (one every 5 px);
 *  - stacked, where the strip is tall enough (VISION): "22 WPM" on one line over the bars, at either size.
 *  None fits: they stay on the spectrum. Only the sizes here, so a host test (tools/cw_strip) can check them.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CW_STRIP_BARS       15      /* as on the spectrum (cw_tune_view.c) */
#define CW_STRIP_BARS_H     40      /* as tall as on the spectrum at most */
#define CW_STRIP_BARS_H_MIN 24      /* lower, the bars at the ends are 4 px: no shape left to read */
#define CW_STRIP_GAP        8       /* between the speed and the bars */

/* The forms, in the order they are tried */
enum { CW_STRIP_NONE, CW_STRIP_ROW, CW_STRIP_STACK };

/* The room and the sizes of what goes in it, in px */
typedef struct {
    int16_t room_w, room_h;     /* the free space in the strip */
    bool    bars, wpm;          /* the bars (CW Tuner on), the speed (CW decoder on) */
    int16_t num_w, num_h;       /* the speed's number in its font: the widest, "50" */
    int16_t unit_w, unit_h;     /* "WPM" in its smaller font, under the number */
    int16_t line_w;             /* "50 WPM" on one line in that smaller font (stacked) */
} cw_strip_in_t;

/* What goes where: the whole w x h, the bars and the speed in it from its top left. A part that is not shown has
 * no size */
typedef struct {
    int     form;
    bool    one_line;           /* the speed as "22 WPM" (stacked), else the number over "WPM" */
    int16_t w, h;
    int16_t bar_w, gap;         /* a bar every bar_w + gap px, gap at the ends (cw_tune_view.c draws them) */
    int16_t bars_x, bars_y, bars_w, bars_h;
    int16_t wpm_x, wpm_y, wpm_w, wpm_h;
} cw_strip_fit_t;

static inline int16_t cw_strip_bars_w(int16_t bar_w, int16_t gap) {
    return CW_STRIP_BARS * (bar_w + gap) + gap;
}

static inline int16_t cw_strip_max(int16_t a, int16_t b) {
    return a > b ? a : b;
}

/* One form at one bar size; false where it does not fit the room */
static inline bool cw_strip_try(const cw_strip_in_t *in, int form, int16_t bar_w, int16_t gap, cw_strip_fit_t *f) {
    int16_t bw = in->bars ? cw_strip_bars_w(bar_w, gap) : 0;

    *f = (cw_strip_fit_t){.form = form, .one_line = form == CW_STRIP_STACK, .bar_w = bar_w, .gap = gap};
    if (in->wpm) {
        f->wpm_w = f->one_line ? in->line_w : cw_strip_max(in->num_w, in->unit_w);
        f->wpm_h = f->one_line ? in->unit_h : in->num_h + in->unit_h;
    }
    if (form == CW_STRIP_ROW) {
        f->bars_h = in->bars ? (in->room_h < CW_STRIP_BARS_H ? in->room_h : CW_STRIP_BARS_H) : 0;
        f->w = f->wpm_w + (in->wpm && in->bars ? CW_STRIP_GAP : 0) + bw;
        f->h = cw_strip_max(f->wpm_h, f->bars_h);
        f->wpm_y = (f->h - f->wpm_h) / 2;
        f->bars_x = f->w - bw;
        f->bars_y = (f->h - f->bars_h) / 2;
    } else {
        /* stacked: the speed's line over the bars, both in the middle */
        int16_t left = in->room_h - f->wpm_h - (in->wpm ? 2 : 0);

        if (!in->wpm || !in->bars) return false;    /* one of them alone is a row */
        f->bars_h = left < CW_STRIP_BARS_H ? left : CW_STRIP_BARS_H;
        f->w = cw_strip_max(f->wpm_w, bw);
        f->h = f->wpm_h + 2 + f->bars_h;
        f->wpm_x = (f->w - f->wpm_w) / 2;
        f->bars_x = (f->w - bw) / 2;
        f->bars_y = f->wpm_h + 2;
    }
    f->bars_w = bw;
    if (!in->bars) f->bar_w = f->gap = 0;
    return f->w <= in->room_w && f->h <= in->room_h && (!in->bars || f->bars_h >= CW_STRIP_BARS_H_MIN);
}

/* The first form that fits; .form CW_STRIP_NONE when none does (or nothing is to be shown) */
static inline cw_strip_fit_t cw_strip_fit(const cw_strip_in_t *in) {
    static const int16_t sizes[2][2] = {{5, 4}, {2, 3}};    /* as on the spectrum, then narrower */
    cw_strip_fit_t       f = {.form = CW_STRIP_NONE};

    if (!in->bars && !in->wpm) return f;
    for (int form = CW_STRIP_ROW; form <= CW_STRIP_STACK; form++)
        for (int s = 0; s < 2; s++)
            if (cw_strip_try(in, form, sizes[s][0], sizes[s][1], &f)) return f;
    return (cw_strip_fit_t){.form = CW_STRIP_NONE};
}

/* The narrowest of them that the strip's height takes, 0 none: what VISION keeps free left of its big digits
 * when they move left for the callsign (freq_strip.c, band_room()) */
static inline int16_t cw_strip_min_w(const cw_strip_in_t *in) {
    cw_strip_in_t  wide = *in;
    cw_strip_fit_t f;
    int16_t        best = 0;

    wide.room_w = INT16_MAX;
    for (int form = CW_STRIP_ROW; form <= CW_STRIP_STACK; form++)
        if (cw_strip_try(&wide, form, 2, 3, &f) && (!best || f.w < best)) best = f.w;
    return best;
}

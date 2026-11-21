/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of how the CW Tuner's bars and the decoder's speed fit the frequency strip
 *  (src/cw_strip_fit.h, #308). With the sizes of DejaVu, the default font, in the 50 px strip and in VISION's 70 px
 *  one: the room of the 20 m band beside 14.100.000 takes the row at the spectrum's size, LOCK in front of the
 *  frequency the row with the narrower bars, a split's two frequencies the stacked form, a split with LOCK none
 *  (back on the spectrum); VISION's room takes the stacked form. The bars alone and the speed alone, nothing to
 *  show. Then every room from 0 to 400 x 0 to 90 px for each of them: what is placed stays inside the room and
 *  inside the whole, the speed and the bars apart, the bars no lower than their least, as tall as on the spectrum
 *  where the row has the height; a wider room never gives a later form; and the narrowest width VISION keeps free
 *  for them is the least room that takes one. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cw_strip_fit.h"

#include <stdio.h>

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* DejaVu: the number in its 24 ("88" 30 px, 21 tall), "WPM" in its 16 (33 px, 14 tall), "88 WPM" 58 px */
static cw_strip_in_t sizes(int16_t room_w, int16_t room_h, bool bars, bool wpm) {
    return (cw_strip_in_t){.room_w = room_w, .room_h = room_h, .bars = bars, .wpm = wpm, .num_w = 30, .num_h = 21,
                           .unit_w = 33, .unit_h = 14, .line_w = 58};
}

/* 0 the row at the spectrum's size, 1 the row narrower, 2 and 3 stacked, 4 none: the order they are tried in */
static int rank(const cw_strip_fit_t *f) {
    if (f->form == CW_STRIP_NONE) return 4;
    return (f->form == CW_STRIP_STACK ? 2 : 0) + (f->bar_w == 2);
}

/* What is placed: inside the room and the whole, the speed and the bars apart */
static bool inside(const cw_strip_in_t *in, const cw_strip_fit_t *f) {
    if (f->form == CW_STRIP_NONE) return true;
    if (f->w > in->room_w || f->h > in->room_h || f->w <= 0 || f->h <= 0) return false;
    if (in->bars) {
        if (f->bars_w != cw_strip_bars_w(f->bar_w, f->gap)) return false;
        if (f->bars_x < 0 || f->bars_y < 0 || f->bars_x + f->bars_w > f->w || f->bars_y + f->bars_h > f->h) return false;
        if (f->bars_h < CW_STRIP_BARS_H_MIN || f->bars_h > CW_STRIP_BARS_H) return false;
    } else if (f->bars_w || f->bars_h) {
        return false;
    }
    if (in->wpm) {
        if (f->wpm_x < 0 || f->wpm_y < 0 || f->wpm_x + f->wpm_w > f->w || f->wpm_y + f->wpm_h > f->h) return false;
        if (in->bars && f->form == CW_STRIP_ROW && f->wpm_x + f->wpm_w + CW_STRIP_GAP > f->bars_x) return false;
        if (in->bars && f->form == CW_STRIP_STACK && f->wpm_y + f->wpm_h > f->bars_y) return false;
    } else if (f->wpm_w || f->wpm_h) {
        return false;
    }
    return true;
}

int main(void) {
    cw_strip_in_t  in;
    cw_strip_fit_t f;

    check(cw_strip_bars_w(5, 4) == 139, "the bars at the spectrum's size: 139 px, as cw_tune_view.c's box");
    check(cw_strip_bars_w(2, 3) == 78, "the narrower bars: 78 px");

    /* The 50 px strip (48 inside its border, 1 px kept at the top and the bottom) */
    in = sizes(200, 46, true, true);    /* "20m" ... 14.100.000 */
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_ROW && f.bar_w == 5 && f.gap == 4 && f.w == 33 + CW_STRIP_GAP + 139,
          "20 m beside 14.100.000: the number over WPM, the bars at the spectrum's size");
    check(f.bars_h == CW_STRIP_BARS_H && f.wpm_h == 35 && !f.one_line, "  the bars as tall as on the spectrum");
    check(f.bars_x == f.w - 139 && f.wpm_x == 0, "  the speed first, the bars at the frequency's side");

    in = sizes(128, 46, true, true);    /* LOCK in front of the frequency */
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_ROW && f.bar_w == 2 && f.w == 33 + CW_STRIP_GAP + 78, "LOCK: the row, the bars narrower");

    in = sizes(90, 46, true, true);     /* a split's two frequencies */
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_STACK && f.bar_w == 2 && f.one_line && f.w == 78 && f.h == 46 && f.bars_h == 30,
          "a split: \"22 WPM\" over the narrower bars, 30 px tall");
    check(f.wpm_x == (78 - 58) / 2 && f.wpm_y == 0 && f.bars_y == 16, "  the line in the middle over the bars");

    in = sizes(18, 46, true, true);     /* a split with LOCK */
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_NONE, "a split with LOCK: none, back on the spectrum");

    /* VISION: 70 px, DejaVu three sizes up (the 24 is a 30: "88" 36 px, 29 tall; the 16 a 22: "WPM" 44, 19 tall) */
    in = (cw_strip_in_t){.room_w = 104, .room_h = 66, .bars = true, .wpm = true, .num_w = 36, .num_h = 29,
                         .unit_w = 44, .unit_h = 19, .line_w = 78};
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_STACK && f.bar_w == 2 && f.w == 78 && f.bars_h == CW_STRIP_BARS_H,
          "VISION, 20 m beside 14.100.000: stacked, the narrower bars as tall as on the spectrum");
    check(cw_strip_min_w(&in) == 78, "VISION keeps 78 px free for them");

    /* One of them alone */
    in = sizes(140, 46, true, false);
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_ROW && f.w == 139 && f.bars_x == 0 && !f.wpm_w, "the bars alone (decoder off)");
    in = sizes(100, 46, true, false);
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_ROW && f.w == 78, "  narrower where 139 px do not fit");
    in = sizes(60, 46, true, false);
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_NONE, "  none under 78 px: stacked is for the two together");
    in = sizes(40, 46, false, true);
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_ROW && f.w == 33 && f.h == 35 && !f.bars_w && !f.bar_w, "the speed alone (Tuner off)");
    in = sizes(32, 46, false, true);
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_NONE, "  none where its 33 px do not fit");
    in = sizes(400, 90, false, false);
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_NONE && cw_strip_min_w(&in) == 0, "neither on: nothing");

    /* Too low for the bars */
    in = sizes(400, CW_STRIP_BARS_H_MIN - 1, true, true);
    f = cw_strip_fit(&in);
    check(f.form == CW_STRIP_NONE, "a strip lower than the bars' least: none");

    /* Every room, each of the three */
    int bad_inside = 0, bad_order = 0, bad_min = 0, bad_tall = 0;

    for (int what = 1; what <= 3; what++) {
        bool bars = what & 1, wpm = what & 2;

        for (int16_t h = 0; h <= 90; h++) {
            int            last = 5;
            cw_strip_in_t  probe = sizes(0, h, bars, wpm);
            int16_t        min_w = cw_strip_min_w(&probe);
            int16_t        first = -1;

            for (int16_t w = 0; w <= 400; w++) {
                in = sizes(w, h, bars, wpm);
                f = cw_strip_fit(&in);
                bad_inside += !inside(&in, &f);
                bad_order += rank(&f) > last;
                last = rank(&f);
                if (f.form != CW_STRIP_NONE && first < 0) first = w;
                /* the row has the height: as tall as on the spectrum */
                if (f.form == CW_STRIP_ROW && bars && h >= CW_STRIP_BARS_H) bad_tall += f.bars_h != CW_STRIP_BARS_H;
            }
            bad_min += first != (min_w ? min_w : -1);
        }
    }
    check(bad_inside == 0, "every room: what is placed inside the room and the whole, the two apart");
    check(bad_order == 0, "every room: a wider one never gives a later form");
    check(bad_tall == 0, "every room: the row's bars as tall as on the spectrum where it has the height");
    check(bad_min == 0, "every height: the narrowest width is the least room that takes one");
    if (bad_inside || bad_order || bad_min || bad_tall) {
        printf("  %d inside, %d order, %d narrowest, %d tall wrong\n", bad_inside, bad_order, bad_min, bad_tall);
    }

    printf("cw_strip: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}

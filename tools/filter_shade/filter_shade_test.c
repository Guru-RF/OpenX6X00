/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of where the filter shade goes (src/filter_shade.h, #250), as the spectrum and the waterfall
 *  cut it: on an 800 px view, the passband over it (Passband) or the columns left and right of it (Outside), with the
 *  passband in the middle, at the view's edges, partly or wholly off it and wider than it, at zoom 1 to 32 for a
 *  2.7 kHz USB passband. Then every passband of a small view against every other: each column shaded or not as the
 *  choice says, the spans inside the view, none empty or overlapping, Passband and Outside together the whole view;
 *  and for a move from one to the other, every column whose Outside shade changed among those filter_shade_moved()
 *  gives, which the waterfall draws again (all of the shade at each move was most of its width at every tuning step
 *  in Fixed mode). See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "filter_shade.h"

#include <stdbool.h>
#include <stdio.h>

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

/* The spans as a string "x1-x2 x1-x2", for the cases written out */
static const char *spans(int where, int32_t left, int32_t right, int32_t x1, int32_t x2) {
    static char         buf[64];
    filter_shade_span_t s[2];
    int                 n = filter_shade_spans(where, left, right, x1, x2, s);
    int                 len = 0;

    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s%d-%d", i ? " " : "", (int)s[i].x1, (int)s[i].x2);
    }
    return buf;
}

static void case_is(const char *what, int where, int32_t x1, int32_t x2, const char *want) {
    const char *got = spans(where, 0, 799, x1, x2);
    char        line[160];
    bool        ok = true;

    for (int i = 0; got[i] || want[i]; i++) {
        if (got[i] != want[i]) {
            ok = false;
            break;
        }
    }
    snprintf(line, sizeof(line), "%s: \"%s\", want \"%s\"", what, got, want);
    check(ok, line);
}

/* Column x shaded by the spans */
static bool in_spans(const filter_shade_span_t *s, int n, int32_t x) {
    for (int i = 0; i < n; i++) {
        if (x >= s[i].x1 && x <= s[i].x2) return true;
    }
    return false;
}

int main(void) {
    const int P = FILTER_SHADE_PASSBAND, O = FILTER_SHADE_OUTSIDE;

    /* An 800 px view, columns 0-799 */
    case_is("middle, Passband", P, 400, 421, "400-421");
    case_is("middle, Outside", O, 400, 421, "0-399 422-799");
    case_is("at the left edge, Outside", O, 0, 21, "22-799");
    case_is("at the right edge, Outside", O, 778, 799, "0-777");
    case_is("partly off the left, Passband", P, -10, 11, "0-11");
    case_is("partly off the left, Outside", O, -10, 11, "12-799");
    case_is("partly off the right, Passband", P, 790, 811, "790-799");
    case_is("partly off the right, Outside", O, 790, 811, "0-789");
    case_is("wholly off the left, Passband", P, -40, -19, "");
    case_is("wholly off the left, Outside", O, -40, -19, "0-799");
    case_is("wholly off the right, Outside", O, 820, 841, "0-799");
    case_is("wider than the view, Passband", P, -100, 900, "0-799");
    case_is("wider than the view, Outside", O, -100, 900, "");
    case_is("one column, Outside", O, 400, 400, "0-399 401-799");
    case_is("edges the wrong way round, Outside", O, 421, 400, "0-399 422-799");
    case_is("the view's whole width, Outside", O, 0, 799, "");

    /* A 2.7 kHz USB passband from the VFO in the middle, as the spectrum places it (8 Hz a pixel at zoom 16): from
     * zoom 32 on wider than the half of the view it is on, cut there */
    for (int zoom = 1; zoom <= 32; zoom *= 2) {
        int32_t             span = 100000 / zoom;
        int32_t             x1 = 400 + (int32_t)((float)(800 * 0) / span + 1.0f);
        int32_t             x2 = 400 + (int32_t)((float)(800 * 2700) / span + 1.0f);
        filter_shade_span_t s[2];
        int                 n = filter_shade_spans(O, 0, 799, x1, x2, s);
        char                what[96];

        snprintf(what, sizeof(what), "zoom %d: the passband (%d-%d) clear, the rest shaded", zoom, (int)x1, (int)x2);
        check(n >= 1 && s[0].x1 == 0 && s[0].x2 == x1 - 1 && (x2 >= 799 ? n == 1 : n == 2 && s[1].x1 == x2 + 1 &&
              s[1].x2 == 799), what);
    }

    /* Every passband of a 24 px view (columns 8-31), from wholly off the left to wholly off the right */
    const int32_t L = 8, R = 31, LO = -4, HI = 44;
    int           bad_cols = 0, bad_spans = 0, bad_moves = 0;

    for (int32_t a = LO; a <= HI; a++) {
        for (int32_t b = a; b <= HI; b++) {
            filter_shade_span_t p[2], o[2];
            int                 np = filter_shade_spans(P, L, R, a, b, p);
            int                 no = filter_shade_spans(O, L, R, a, b, o);

            for (int i = 0; i < np; i++) {
                bad_spans += p[i].x1 > p[i].x2 || p[i].x1 < L || p[i].x2 > R;
            }
            for (int i = 0; i < no; i++) {
                bad_spans += o[i].x1 > o[i].x2 || o[i].x1 < L || o[i].x2 > R || (i && o[i].x1 <= o[i - 1].x2);
            }
            for (int32_t x = L; x <= R; x++) {
                bool inside = x >= a && x <= b;

                bad_cols += in_spans(p, np, x) != inside || in_spans(o, no, x) == inside;
            }
            /* A move to every other passband: the columns whose Outside shade changed are among those drawn again */
            for (int32_t c = LO; c <= HI; c++) {
                for (int32_t d = c; d <= HI; d++) {
                    filter_shade_span_t m[2], o2[2];
                    int                 nm = filter_shade_moved(a, b, c, d, m);
                    int                 no2 = filter_shade_spans(O, L, R, c, d, o2);

                    for (int32_t x = L; x <= R; x++) {
                        if (in_spans(o, no, x) != in_spans(o2, no2, x) && !in_spans(m, nm, x)) {
                            bad_moves++;
                        }
                    }
                    bad_moves += (a == c && b == d) != (nm == 0);    /* no move: nothing drawn again */
                }
            }
        }
    }
    check(bad_spans == 0, "every passband: the spans inside the view, none empty, Outside's two apart");
    check(bad_cols == 0, "every passband: Passband shades it and only it, Outside all but it");
    check(bad_moves == 0, "every move: each column whose Outside shade changed is drawn again, none for no move");
    if (bad_cols || bad_spans || bad_moves) {
        printf("  %d columns, %d spans, %d moves wrong\n", bad_cols, bad_spans, bad_moves);
    }

    /* A move in Fixed mode, a 10 Hz step at zoom 16 (1 px), the passband 2.7 kHz wide: two columns drawn again, not
     * the 750-odd of the shade */
    filter_shade_span_t m[2];
    int                 nm = filter_shade_moved(400, 738, 401, 739, m);
    int32_t             cols = 0;

    for (int i = 0; i < nm; i++) cols += m[i].x2 - m[i].x1 + 1;
    check(nm == 2 && cols == 4, "a 1 px move: 4 columns drawn again (2 at each edge)");

    printf("filter_shade: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}

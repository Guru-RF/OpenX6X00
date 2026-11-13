/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - where the filter shade goes on the spectrum and the waterfall (#167, #250)
 *
 *  Settings > Display > SPECTRUM: Filter shade sets how strongly (Edges, 25 %, 50 %, 75 %, Full) and Shaded part
 *  where: over the passband, as up to v0.3.11, or outside it, the passband clear and the rest of the view dimmed.
 *  Outside is two rectangles, left and right of the passband, at the strength the passband's shade has: no work in
 *  the waterfall's or the trace's own pictures. Edges draws only the passband's two edges whichever part is chosen,
 *  and the 1bit theme always does. Here only the columns, so the spectrum (spectrum_view.c) and the waterfall
 *  (waterfall_view.c) cut them the same way and a host test (tools/filter_shade) can check them.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdint.h>

/* Shaded part (the filter_shade_where parameter) */
enum { FILTER_SHADE_PASSBAND, FILTER_SHADE_OUTSIDE };

/* Columns x1 to x2, both shaded */
typedef struct {
    int32_t x1, x2;
} filter_shade_span_t;

/* The columns to shade on a view from left to right (both in) with the passband on columns pass_x1 to pass_x2 (both
 * in): the passband's, or with Outside those left and right of it. Cut to the view: a passband at the view's edge,
 * partly or wholly off it, or wider than it, leaves only what is on it; Outside then shades the whole view, or the
 * side that is on it, or nothing. The count of spans put in out, 0 to 2, none of them empty */
static inline int filter_shade_spans(int where, int32_t left, int32_t right, int32_t pass_x1, int32_t pass_x2,
                                     filter_shade_span_t out[2]) {
    int n = 0;

    if (pass_x1 > pass_x2) {
        int32_t t = pass_x1;

        pass_x1 = pass_x2;
        pass_x2 = t;
    }
    if (where != FILTER_SHADE_OUTSIDE) {
        out[n] = (filter_shade_span_t){pass_x1 > left ? pass_x1 : left, pass_x2 < right ? pass_x2 : right};
        n += out[n].x1 <= out[n].x2;
        return n;
    }
    out[n] = (filter_shade_span_t){left, pass_x1 - 1 < right ? pass_x1 - 1 : right};
    n += out[n].x1 <= out[n].x2;
    out[n] = (filter_shade_span_t){pass_x2 + 1 > left ? pass_x2 + 1 : left, right};
    n += out[n].x1 <= out[n].x2;
    return n;
}

/* Outside, the passband moved from columns old_x1..old_x2 to new_x1..new_x2: the columns whose shade may have
 * changed, those between the old and the new left edge and between the old and the new right edge, not cut to a
 * view. All of the shade drawn again at each move was most of the waterfall's width at every tuning step in Fixed
 * mode, where the passband moves across a still view. The count put in out, 0 to 2 */
static inline int filter_shade_moved(int32_t old_x1, int32_t old_x2, int32_t new_x1, int32_t new_x2,
                                     filter_shade_span_t out[2]) {
    int n = 0;

    if (old_x1 != new_x1) {
        out[n++] = (filter_shade_span_t){old_x1 < new_x1 ? old_x1 : new_x1, old_x1 < new_x1 ? new_x1 : old_x1};
    }
    if (old_x2 != new_x2) {
        out[n++] = (filter_shade_span_t){old_x2 < new_x2 ? old_x2 : new_x2, old_x2 < new_x2 ? new_x2 : old_x2};
    }
    return n;
}

/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's wide FM filter as a width (#283)
 *
 *  WFM sends the base the band from -edge to +edge (radio.c, on_high_filter_change: low = -high). The X6200's base
 *  (1.0.8) takes the two edges as one width, high - low, with nothing cut below, and goes no wider than 16000 Hz: a
 *  wider one is set to 16000. The stock firmware sets WFM to -8000..+8000 and shows it as 16 kHz, the width, as it
 *  shows its AM filters (9, 12 and 15 kHz are -4500..+4500 and so on). The GUI showed WFM's edge as its width: 8000 Hz
 *  where the stock firmware says 16 kHz, up to 10000, which the base cut to 16 kHz as well. Here the width the knob,
 *  the button and the info bar show for WFM, twice the edge, and the edge kept for a width; up to the base's 16 kHz.
 *  AM, NFM and SAM still show their edge as the width, as the X6100 does.
 *  Header only, so a host test (tools/wfm_filter) checks it against the base's way.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdint.h>

#define WFM_BW_MAX  16000   /* Hz: the base's widest, the stock firmware's 16 kHz */
#define WFM_BW_MIN  20      /* Hz: an edge of 10 Hz, the narrowest the knob leaves */

/* The width of an edge: both sides of the dial */
static inline int32_t wfm_bw_of_edge(int32_t edge) {
    return 2 * edge;
}

/* The edge kept for a width, the width brought into WFM_BW_MIN..WFM_BW_MAX first; an odd one loses its last hertz */
static inline int32_t wfm_edge_of_bw(int32_t bw) {
    if (bw < WFM_BW_MIN) bw = WFM_BW_MIN;
    if (bw > WFM_BW_MAX) bw = WFM_BW_MAX;
    return bw / 2;
}

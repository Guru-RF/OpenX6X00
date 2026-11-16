/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the X6200's wide FM filter as a width (src/wfm_filter.h, #283), against the base's way
 *  with its rx filter register: the default the stock firmware's 16 kHz, every width the knob gives reaching the base
 *  as it is (none cut, no two the same), the knob stopping at both ends, and the edges of up to v0.3.12 (8 to 10 kHz)
 *  all one width at the base. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "wfm_filter.h"

#include <stdbool.h>
#include <stdio.h>

static int checks, failed;

static void check(bool ok, const char *what, long got, long want) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s: %ld, want %ld\n", what, got, want);
    }
}

#define EQ(what, got, want) check((long)(got) == (long)(want), what, (long)(got), (long)(want))

/* The register as the X6200 control library packs it (x6200_control_rx_filter_set_low/_high): the high edge in the
 * low 16 bits, the low edge in the high 16, both as int16 */
static uint32_t reg(int32_t low, int32_t high) {
    return ((uint32_t)(uint16_t)(int16_t)low << 16) | (uint16_t)(int16_t)high;
}

/* The base (1.0.8) with that register, as it behaves: what it cuts below (hpf) and the width up to which it passes
 * (lpf). A low edge under 0 (AM, SAM, NFM, WFM): nothing cut below, the width high - low; a width over 16000 Hz is
 * 16000. A high edge of 0 or less is refused: false, the filter it had stays */
static bool base_filter(uint32_t r, int32_t *hpf, int32_t *lpf) {
    int32_t high = (int16_t)(r & 0xFFFF), low = (int16_t)(r >> 16);

    if (high <= 0) return false;
    if (low < 0) {
        *hpf = 0;
        *lpf = (int16_t)(uint16_t)(high - low);
    } else {
        *hpf = low;
        *lpf = high;
    }
    if (*lpf >= 16000) *lpf = 16000;
    return true;
}

/* What the base passes for the width the GUI shows: the edge kept, sent as radio.c sends it (-edge, +edge) */
static int32_t base_width(int32_t shown) {
    int32_t edge = wfm_edge_of_bw(shown), hpf = -1, lpf = -1;

    if (!base_filter(reg(-edge, edge), &hpf, &lpf) || hpf != 0) return -1;
    return lpf;
}

int main(void) {
    int32_t hpf, lpf;

    /* The stock firmware's AM presets, 9, 12 and 15 kHz as it shows them, and its WFM, 16 kHz: widths at the base */
    base_filter(reg(-4500, 4500), &hpf, &lpf);
    EQ("stock AM 9 kHz at the base", lpf, 9000);
    base_filter(reg(-7500, 7500), &hpf, &lpf);
    EQ("stock AM 15 kHz at the base", lpf, 15000);
    base_filter(reg(-8000, 8000), &hpf, &lpf);
    EQ("stock WFM 16 kHz at the base", lpf, 16000);
    EQ("stock WFM: nothing cut below", hpf, 0);

    /* The default edge (settings_manager.cpp, MODE_GROUP_WFM 8000) shows as the stock firmware's 16 kHz, the widest */
    EQ("default edge 8000 shown", wfm_bw_of_edge(8000), 16000);
    EQ("the widest is the base's", WFM_BW_MAX, 16000);
    EQ("the default at the base", base_width(wfm_bw_of_edge(8000)), 16000);

    /* Every width on a 20 Hz grid (the knob's WFM detent is 100 Hz, radio_controls.cpp) reaches the base as shown:
     * none cut, so no two the same */
    int bad = 0, first_bad = 0;

    for (int32_t w = WFM_BW_MIN; w <= WFM_BW_MAX; w += 20) {
        if (base_width(w) != w || wfm_bw_of_edge(wfm_edge_of_bw(w)) != w) {
            if (!bad++) first_bad = w;
        }
    }
    EQ("knob widths the base passes as they are (first wrong one)", bad ? first_bad : 0, 0);

    /* The knob's ends: a detent past either keeps the end (radio_controls.cpp sets the width shown +- 100) */
    EQ("a detent up from the widest", wfm_bw_of_edge(wfm_edge_of_bw(WFM_BW_MAX + 100)), WFM_BW_MAX);
    EQ("a detent down from the narrowest", wfm_bw_of_edge(wfm_edge_of_bw(WFM_BW_MIN - 100)), WFM_BW_MIN);
    EQ("nothing at all", wfm_bw_of_edge(wfm_edge_of_bw(0)), WFM_BW_MIN);
    EQ("an odd width loses its last hertz", wfm_bw_of_edge(wfm_edge_of_bw(12345)), 12344);

    /* Up to v0.3.12 the edge went to 10000 and showed as the width: 8000 to 10000 were all the base's 16 kHz, the
     * "10 kHz" of #283 was the stock firmware's 16 kHz already. Such an edge now comes back as the widest */
    for (int32_t e = 8000; e <= 10000; e += 500) {
        base_filter(reg(-e, e), &hpf, &lpf);
        EQ("an old edge at the base", lpf, 16000);
    }
    EQ("old edge 10000 now", wfm_bw_of_edge(wfm_edge_of_bw(wfm_bw_of_edge(10000))), WFM_BW_MAX);

    /* The base refuses a filter with no high edge: the GUI never sends one in WFM */
    EQ("no high edge refused", base_filter(reg(0, 0), &hpf, &lpf), false);
    EQ("the narrowest at the base", base_width(WFM_BW_MIN), WFM_BW_MIN);

    printf("wfm_filter: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}

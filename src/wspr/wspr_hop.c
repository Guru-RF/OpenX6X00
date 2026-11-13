/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WSPR band hopping (wspr_hop.h, #190)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "wspr_hop.h"

#include "../cfg/ham_bands.h"

#include <stdlib.h>

/* WSJT-X's WSPR dial frequencies, as sql/params.sql has them, in the order of the slots */
static const struct {
    int     m;
    int32_t dial;
} bands_tab[WSPR_HOP_BANDS] = {
    {160, 1836600}, {80, 3568600},   {60, 5287200},   {40, 7038600},   {30, 10138700}, {20, 14095600},
    {17, 18104600}, {15, 21094600},  {12, 24924600},  {10, 28124600},  {6, 50293000},
};

int wspr_hop_band_m(int band) {
    return band >= 0 && band < WSPR_HOP_BANDS ? bands_tab[band].m : 0;
}

int32_t wspr_hop_band_dial(int band) {
    return band >= 0 && band < WSPR_HOP_BANDS ? bands_tab[band].dial : 0;
}

/* By the amateur band's edges, not by the nearest WSPR frequency: a frequency of the user's own on a band (60 m has
 * more than one in use) is that band, one between two bands is none */
int wspr_hop_band_of(int32_t hz) {
    int32_t lo, hi;

    if (!ham_band_of(hz, &lo, &hi)) {
        return -1;
    }
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        if (bands_tab[i].dial >= lo && bands_tab[i].dial <= hi) {
            return i;
        }
    }
    return -1;
}

wspr_use_t wspr_hop_use(int32_t bands, int band) {
    if (band < 0 || band >= WSPR_HOP_BANDS) {
        return WSPR_USE_OFF;
    }
    switch ((bands >> (2 * band)) & 3) {
        case 1:
            return WSPR_USE_RX;
        case 2:
            return WSPR_USE_TX;
        default:
            return WSPR_USE_OFF;
    }
}

int32_t wspr_hop_set_use(int32_t bands, int band, wspr_use_t use) {
    if (band < 0 || band >= WSPR_HOP_BANDS) {
        return bands;
    }
    bands &= ~(3 << (2 * band));
    return bands | ((int32_t)use & 3) << (2 * band);
}

bool wspr_hop_any(int32_t bands, wspr_use_t use) {
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        if (wspr_hop_use(bands, i) >= use) {
            return true;
        }
    }
    return false;
}

wspr_use_t wspr_hop_next_use(wspr_use_t use) {
    return use == WSPR_USE_OFF ? WSPR_USE_RX : use == WSPR_USE_RX ? WSPR_USE_TX : WSPR_USE_OFF;
}

const char *wspr_hop_use_name(wspr_use_t use) {
    return use == WSPR_USE_TX ? "RX+TX" : use == WSPR_USE_RX ? "RX" : "Off";
}

int wspr_hop_slot(time_t cycle) {
    long minute = (long)(((cycle % 3600) + 3600) % 3600) / 60;

    return (int)(minute % 20) / 2;
}

int wspr_hop_pick(int32_t bands, time_t cycle) {
    int slot = wspr_hop_slot(cycle);
    int on[WSPR_HOP_BANDS], n = 0;
    int offs = 0, before = 0;

    if (wspr_hop_use(bands, slot) != WSPR_USE_OFF) {
        return slot;
    }
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        if (wspr_hop_use(bands, i) != WSPR_USE_OFF) {
            on[n++] = i;
        }
    }
    if (n == 0) {
        return -1;
    }
    /* The Off slots counted from the top of the hour, 20 minutes after 20 minutes: the k-th goes to the k-th band
     * that is on, round and round, so each gets the same share of them */
    for (int s = 0; s < WSPR_HOP_SLOTS; s++) {
        if (wspr_hop_use(bands, s) == WSPR_USE_OFF) {
            offs++;
            before += s < slot;
        }
    }
    long long block = (long long)cycle / (WSPR_HOP_SLOTS * WSPR_HOP_CYCLE);

    return on[(block * offs + before) % n];
}

bool wspr_hop_tx_ok(int32_t bands, int band, bool atu_on, bool atu_match) {
    return wspr_hop_use(bands, band) == WSPR_USE_TX && (!atu_on || atu_match);
}

bool wspr_hop_window(double t, bool on_air, bool recording) {
    return !on_air && !recording && t >= WSPR_HOP_FROM_S && t < WSPR_HOP_TO_S;
}

void wspr_hop_freqs(const int32_t *presets, int n, int32_t freqs[WSPR_HOP_BANDS]) {
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        freqs[i] = 0;
    }
    for (int k = 0; k < n; k++) {
        int b = wspr_hop_band_of(presets[k]);

        if (b >= 0 && (!freqs[b] || labs((long)presets[k] - bands_tab[b].dial) < labs((long)freqs[b] - bands_tab[b].dial))) {
            freqs[b] = presets[k];
        }
    }
}

int32_t wspr_hop_usable(int32_t bands, const int32_t freqs[WSPR_HOP_BANDS]) {
    for (int i = 0; i < WSPR_HOP_BANDS; i++) {
        if (!freqs[i]) {
            bands = wspr_hop_set_use(bands, i, WSPR_USE_OFF);
        }
    }
    return bands;
}

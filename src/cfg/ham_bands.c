/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The amateur TX bands (ham_bands.h, #121). Every edge but the Alaska window's is a real allocation edge somewhere,
 *  none rounded outward. Sources (2026-11-02): ITU RR Art. 5 and its footnotes; 47 CFR 97.301, 97.303, 97.401 (US);
 *  RBR-4 Issue 3 (Canada); OFW611 (UK); ComReg 09/45R5 (Ireland); IARU R1 VHF Handbook 10.03; the JARL band plan of
 *  17 Jul 2025; ACMA's LCD (Australia). Left out, the switch covers them: Ireland's 30-49 and 54-69.9 MHz blocks
 *  outside 8 m, Somalia's 5060-5250 kHz, Japan's 4630 kHz emergency channel.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "ham_bands.h"

#include <stddef.h>

/* Hz, both edges in; sorted, none touching (tools/ham_bands checks both) */
static const struct {
    int32_t lo, hi;
} bands[] = {
    {135700, 137800},       /* 2200 m: ITU all Regions (RR 5.67A) */
    {472000, 479000},       /* 630 m: ITU all Regions (RR 5.80A) */
    {1800000, 2000000},     /* 160 m: ITU R2/R3; R1 from 1810, to 2000 by RR 5.96 (UK, IE, DE, ...) */
    {3500000, 4000000},     /* 80 m: ITU R2 to 4000 (R1 3800, R3 3900) */
    {5167000, 5168000},     /* Alaska emergency channel (47 CFR 97.401): a 1 kHz window round its carrier 5167.5 kHz,
                             * the dial in USB */
    {5250000, 5450000},     /* 60 m: national 5250-5450 (DK, BG, GL, TT, ...); ITU 5351.5-5366.5 (RR 5.133B);
                             * US, CA, UK, IE inside */
    {7000000, 7300000},     /* 40 m: ITU R2 (RR 5.142) and VK to 7300 (R1/R3 7200) */
    {10100000, 10150000},   /* 30 m: ITU all Regions */
    {14000000, 14350000},   /* 20 m: ITU all Regions */
    {18068000, 18168000},   /* 17 m: ITU all Regions */
    {21000000, 21450000},   /* 15 m: ITU all Regions */
    {24890000, 24990000},   /* 12 m: ITU all Regions */
    {28000000, 29700000},   /* 10 m: ITU all Regions */
    {40000000, 45000000},   /* 8 m: the Irish band plan 40-45 (ComReg 09/45R5, IARU R1 VHF Handbook 10.03); the ISM
                             * 40.66-40.70 elsewhere (SI, ZA, HR, KY, BE, IT; ES 40.65-40.75) */
    {50000000, 54000000},   /* 6 m: ITU R2/R3; R1 50-52, 50-54 by RR 5.169 */
};

bool ham_band_of(int32_t hz, int32_t *lo, int32_t *hi) {
    for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
        if (hz >= bands[i].lo && hz <= bands[i].hi) {
            if (lo) *lo = bands[i].lo;
            if (hi) *hi = bands[i].hi;
            return true;
        }
    }
    return false;
}

/* A transverter's range starts at 70 MHz (its from's minimum), above every band here */
ham_tx_t ham_tx_check(int32_t hz, bool hw_tx, bool xvtr, bool any) {
    if (!hw_tx) return HAM_TX_RX_ONLY;
    if (xvtr || any || ham_band_of(hz, NULL, NULL)) return HAM_TX_OK;
    return HAM_TX_OFF_BAND;
}

/* For the host test: the table itself */
int ham_bands_count(void) {
    return (int)(sizeof(bands) / sizeof(bands[0]));
}

void ham_bands_get(int i, int32_t *lo, int32_t *hi) {
    *lo = bands[i].lo;
    *hi = bands[i].hi;
}

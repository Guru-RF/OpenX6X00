/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's attenuator on a band never set (smeter_att.h, #125): what two X6200s showed on a steady
 *  carrier, by the middle of the band. Measure gives a band its own, and a band set keeps it when these change.
 *  Under 2.5 MHz, 160 m: 59 dB, one radio's only figure (base 1.0.8, 5 November), Measure at S9+19 after a first
 *  run at S9+18 ended "Not steady" with ATT on.
 *  2.5 to 5 MHz, 80 m: 51 dB, in the middle of what was seen there: 51 and 39 dB on that radio (1 and 5 November,
 *  the 39 checked with ATT on and off at S9+20), 40 and about 62 dB on the other (base 1.0.7, 3 and 4 November).
 *  Each radio gave two figures on two days, 12 and 22 dB apart, and what changes on 80 m isn't known yet.
 *  From 5 MHz up one figure, 34 dB: each radio is close to flat there, and they differ by about 14 dB. The first
 *  took 24 to 30 dB off on eight bands from 60 m to 10 m (5 November), about 27; the other about 41, from 40 dB on
 *  40 m (Measure at -55 dBm at v0.3.8's top, and the readings of 3 November), 43 on 20 m and 40 on 10 m.
 *  Outside the bands 40 m's
 */

#include "smeter_att.h"

int32_t smeter_att_start_band(uint32_t start_hz, uint32_t stop_hz) {
    uint32_t hz = start_hz / 2 + stop_hz / 2;

    if (hz == 0) hz = 7100000;          /* outside the bands: 40 m's */
    if (hz < 2500000) return 59;        /* 160 m */
    if (hz < 5000000) return 51;        /* 80 m, the 75 m and 60 m broadcast bands too */
    return 34;                          /* 60 m and up */
}

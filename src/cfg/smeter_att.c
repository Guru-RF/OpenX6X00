/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's attenuator on a band never set (smeter_att.h, #125): what one X6200 showed on a steady
 *  carrier, by the middle of the band, on each power. Measure gives a band its own, and a band set keeps it when
 *  these change.
 *  On an external supply 16 dB on every band: with a 13.5 V supply in, 15 to 16 dB on 80 m and 40 m at -55, -65 and
 *  -75 dBm, from 5 to 61 minutes after a cold start, 16 on 20 m and 15 on 10 m (base 1.0.8, 5 and 6 November); Xiegu's
 *  own firmware took 16 to 17 dB off there too.
 *  On the battery alone, where the same radio took more off:
 *  under 2.5 MHz, 160 m: 59 dB, its only figure, Measure on battery on 4 November.
 *  2.5 to 5 MHz, 80 m: 34 dB, the mean of the four readings on battery on 6 November, 31 to 35 dB, after a cold start
 *  and after the supply went out, on both firmwares; Measure gave 39 on 4 November, and 51 on 1 November.
 *  From 5 MHz up one figure, 28 dB, the mean of the fifteen on battery from 60 m to 10 m: Measure gave 24 to 30 dB on
 *  eight bands on 4 November and 29 to 34 on three on 1 November, and 40 m read 27 four times on 6 November.
 *  Outside the bands 40 m's.
 *  The other X6200 in #125 took about 40 dB off from 5 MHz up and 40 to about 62 on 80 m (base 1.0.7, 1 to 4 November),
 *  on a power not known: it is left out until it is.
 *  And the parts of 40 m and 20 m that share the S-meter figures, by frequency (smeter_share_group, smeter_att.h)
 */

#include "smeter_att.h"

int32_t smeter_att_start_band(uint32_t start_hz, uint32_t stop_hz, bool battery) {
    uint32_t hz = start_hz / 2 + stop_hz / 2;

    if (!battery) return 16;            /* on a supply: every band */
    if (hz == 0) hz = 7100000;          /* outside the bands: 40 m's */
    if (hz < 2500000) return 59;        /* 160 m */
    if (hz < 5000000) return 34;        /* 80 m, the 75 m and 60 m broadcast bands too */
    return 28;                          /* 60 m and up */
}

int32_t smeter_att_now(int32_t supply_db, int32_t battery_db, bool battery) {
    return battery ? battery_db : supply_db;
}

int32_t smeter_share_group(uint32_t start_hz, uint32_t stop_hz) {
    uint32_t hz = start_hz / 2 + stop_hz / 2;

    if (hz >= 7000000 && hz < 7300000) return 40;
    if (hz >= 14000000 && hz < 14350000) return 20;
    return 0;
}

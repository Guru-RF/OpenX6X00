/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's attenuator on a band never set (smeter_att.h, #125): the mean of what two X6200s showed on
 *  a steady carrier, each band taking that of the nearest of 80, 40, 20 and 10 m by the middle of the band. Measure
 *  gives a band its own, and a band set keeps it when these change.
 *  One radio (base 1.0.8): 51 dB on 80 m, the same at S9 and S9+10 and with the AGC off; 29 on 40 m, where Measure
 *  gave 28 twice; 34 on 20 m and 32 on 10 m. The other (base 1.0.7): 40 dB on 80 m and on 40 m, where with ATT on and
 *  the figure at 40 the meter followed a generator 10 dB for 10 dB from -76 to -36 dBm (3 November). Left out: that
 *  radio's S9 table of 1 November (53, 36, 43 and 40 dB), off from its 3 November readings on both bands they share
 *  (13 dB over on 80 m, 4 under on 40 m), and Measure runs that ended "Not steady" or at v0.3.8's top, 40 dB.
 *  So 20 m and 10 m are one radio's figures for now
 */

#include "smeter_att.h"

int32_t smeter_att_start_band(uint32_t start_hz, uint32_t stop_hz) {
    uint32_t hz = start_hz / 2 + stop_hz / 2;

    if (hz == 0) hz = 7100000;          /* outside the bands: 40 m's */
    if (hz < 5000000) return 46;        /* 80 m: 160 m and the broadcast bands up to 60 m too */
    if (hz < 10000000) return 35;       /* 40 m: 60 m to the 31 m broadcast band */
    if (hz < 20000000) return 34;       /* 20 m: 30 m to the 15 m broadcast band */
    return 32;                          /* 10 m: 15 m and up */
}

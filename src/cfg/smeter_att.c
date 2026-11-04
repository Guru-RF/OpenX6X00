/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's attenuator on a band never set (smeter_att.h, #125): what two X6200s showed on a steady
 *  carrier, 53 and 51 dB on 80 m, 36 and 29 on 40 m, 43 and 34 on 20 m, 40 and 32 on 10 m, their mean, each band
 *  taking that of the nearest of the four by the middle of the band. Measure gives a band its own.
 */

#include "smeter_att.h"

int32_t smeter_att_start_band(uint32_t start_hz, uint32_t stop_hz) {
    uint32_t hz = start_hz / 2 + stop_hz / 2;

    if (hz == 0) hz = 7100000;          /* outside the bands: 40 m's */
    if (hz < 5000000) return 52;        /* 80 m: 160 m and the broadcast bands up to 60 m too */
    if (hz < 10000000) return 33;       /* 40 m: 60 m to the 31 m broadcast band */
    if (hz < 20000000) return 39;       /* 20 m: 30 m to the 15 m broadcast band */
    return 36;                          /* 10 m: 15 m and up */
}

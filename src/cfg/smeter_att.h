/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the S-meter on a band never set (#125): the calibration it starts from on either radio, and the
 *  X6200's attenuator figure, which the S-meter adds back with ATT on (smeter.c) until the band has its own, from
 *  Settings > TX Cal > S-METER or its Measure (p_band_smeter_att). tools/smeter_att checks them on the band list,
 *  against the top and against the manual's figures, table and range.
 */

#pragma once

#include <stdint.h>

/* Per band S-meter cal on a band never calibrated, dB (p_smeter_cal, p_band_smeter_cal). The X6100's is what one
 * X6100 needed next to a calibrated transceiver on the same antenna: it works its reading out from the receiver's own
 * samples. The X6200 shows the level its base sends, another scale, and with the X6100's -22 read 30 to 35 dB low, an
 * S9 carrier S3 (#125). Its start is the mean of two X6200s' calibrations with a generator at -73 dBm, 14 figures
 * from +7 to +14 dB on ten bands from 160 m to 10 m (11.2), all of them within 4 dB of it */
#define SMETER_CAL_START_X6100 (-22)
#define SMETER_CAL_START_X6200 11
#ifdef BOARD_X6200
#define SMETER_CAL_START SMETER_CAL_START_X6200
#else
#define SMETER_CAL_START SMETER_CAL_START_X6100
#endif

/* The figure's top, dB: the band's setting stops there, and with it the slider on Settings > TX Cal > S-METER and
 * Measure. One X6200 took about 62 dB off on 80 m (#125), past the 60 of before */
#define SMETER_ATT_MAX 70

#ifdef __cplusplus
extern "C" {
#endif

/* dB, for a band from start_hz to stop_hz, by its middle. 0 and 0: outside the bands (medium wave, the 120 m and 90 m
 * broadcast bands, the gaps between the bands), which share one value */
int32_t smeter_att_start_band(uint32_t start_hz, uint32_t stop_hz);

#ifdef __cplusplus
}
#endif

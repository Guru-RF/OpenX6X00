/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the S-meter on a band never set (#125): the calibration it starts from, on both radios, and the
 *  X6200's attenuator figures, which the S-meter adds back with ATT on (smeter.c) until the band has its own, from
 *  Settings > TX Cal > S-METER or its Measure (p_band_smeter_att, p_band_smeter_att_bat): one on an external supply,
 *  one on the battery alone, and which of the two the meter takes; and the bands that share them, the CW and SSB
 *  parts of 40 m and 20 m. tools/smeter_att checks them on the band list, against the top and against the manual's
 *  figures, table and range.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Per band S-meter cal on a band never calibrated, dB (p_smeter_cal, p_band_smeter_cal), the same on both radios:
 * what one X6100 needed next to a calibrated transceiver on the same antenna. Two X6200s calibrated with a generator
 * at -73 dBm wanted +7 to +14 dB at S9 on ten bands, but the X6200 started at their mean, +11, read about -85 dBm,
 * S7, on every band with no antenna and into a dummy load, against -118 to -121 at -22 (#125): one value lifts the
 * whole scale, and the X6200's meter isn't linear enough for one to set both S9 and the floor */
#define SMETER_CAL_START (-22)

/* The figure's top, dB: the band's setting stops there, and with it the slider on Settings > TX Cal > S-METER and
 * Measure. One X6200 took about 62 dB off on 80 m (#125), past the 60 of before */
#define SMETER_ATT_MAX 70

#ifdef __cplusplus
extern "C" {
#endif

/* The X6200's attenuator takes off more on the battery alone than on an external supply: on one radio 16 dB on 80 m
 * and 40 m with a 13.5 V supply in, 31 to 35 dB on 80 m and 27 on 40 m on its battery, changing the moment the supply
 * went in or out, on Xiegu's firmware too, while without ATT the reading stayed the same (#125). So each band keeps
 * two figures, and the meter takes the one for the power the radio is on. Their band rows: the figure on a supply
 * keeps the row of the one figure of before, so a figure measured or set on v0.3.9 stays, as the one on a supply:
 * the figures #125 tells were stored band by band, 16 dB on 13 bands, were set with the supply in. The figure on
 * battery has a row of its own */
#define SMETER_ATT_ROW      "smeter_att"
#define SMETER_ATT_BAT_ROW  "smeter_att_bat"

/* dB, on an external supply or on the battery alone (battery), for a band from start_hz to stop_hz, by its middle.
 * 0 and 0: outside the bands (medium wave, the 120 m and 90 m broadcast bands, the gaps between the bands), which
 * share one value */
int32_t smeter_att_start_band(uint32_t start_hz, uint32_t stop_hz, bool battery);

/* The figure the S-meter adds back with ATT on: supply_db on an external supply, battery_db on the battery alone */
int32_t smeter_att_now(int32_t supply_db, int32_t battery_db, bool battery);

/* The amateur band whose parts share the S-meter cal and the X6200's attenuator figures, for a band from start_hz to
 * stop_hz by its middle: 40 for 7.000 to 7.300 MHz (40m CW and 40m SSB), 20 for 14.000 to 14.350 MHz (20m CW and
 * 20m SSB), 0 for a band that shares with none. The receiver is the same across each, and DL9RP's calibration at
 * 7.100 left 7.020 at the start (#125). By frequency, not by id or name: the web page renames a band and moves its
 * edges in place, and adds bands, so an id or a name can stand for another band. Broadcast 41m (7.200 to 7.500)
 * keeps its own; 40m SSB to 7.300 in Region 2 still shares */
int32_t smeter_share_group(uint32_t start_hz, uint32_t stop_hz);

#ifdef __cplusplus
}
#endif

/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's attenuator figure on a band never set (#125): the S-meter adds it back with ATT on
 *  (smeter.c) until the band has its own, from Settings > TX Cal > S-METER or its Measure (p_band_smeter_att).
 *  tools/smeter_att checks it on the band list, against the top and against the manual's table and range.
 */

#pragma once

#include <stdint.h>

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

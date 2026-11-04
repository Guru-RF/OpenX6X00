/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the X6200's attenuator figure on a band never set (#125): the S-meter adds it back with ATT on
 *  (smeter.c) until the band has its own, from Settings > TX Cal > S-METER or its Measure (p_band_smeter_att).
 *  tools/smeter_att checks it on the band list and against the manual's table.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dB, for a band from start_hz to stop_hz, by its middle. 0 and 0: outside the bands (medium wave, the 120 m and 90 m
 * broadcast bands, the gaps between the bands), which share one value */
int32_t smeter_att_start_band(uint32_t start_hz, uint32_t stop_hz);

#ifdef __cplusplus
}
#endif

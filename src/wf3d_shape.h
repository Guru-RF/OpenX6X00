/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The 3D waterfall's trace heights from a row's levels (waterfall_view.c): no LVGL, tested on the PC
 *  (tools/wf3d)
 */

#pragma once

#include <stdint.h>

/* Levels above the floor by more than this on the raw row: a signal, its peak kept through the smoothing */
#define WF3D_SIG 10

/* Levels (0..255, the waterfall's colour indexes) of one row, n pixels, to trace heights 0..255: smoothed (7 taps,
 * 1 6 15 20 15 6 1 / 64); the noise (at or below floor) pressed into 0..base; above the floor base + (v - floor) *
 * scale / 256, clamped to 255; where the raw level stands more than WF3D_SIG above the floor the height is at
 * least the raw level's own (peaks kept: a one-pixel CW signal at its height, the noise's spikes smoothed) */
void wf3d_shape(const uint8_t *raw, int n, uint8_t floor, int32_t base, int32_t scale, uint8_t *out);

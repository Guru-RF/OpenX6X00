/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The 3D waterfall's trace heights from a row's levels
 */

#include "wf3d_shape.h"

static inline int32_t height(int32_t v, int32_t floor, int32_t base, int32_t scale) {
    if (v <= floor) {
        return floor > 0 ? v * base / floor : base;
    }
    int32_t h = base + (((v - floor) * scale) >> 8);

    return h > 255 ? 255 : h;
}

void wf3d_shape(const uint8_t *raw, int n, uint8_t floor, int32_t base, int32_t scale, uint8_t *out) {
    static const int32_t k[7] = {1, 6, 15, 20, 15, 6, 1};

    for (int i = 0; i < n; i++) {
        int32_t sum = 0;

        for (int j = -3; j <= 3; j++) {
            int p = i + j;

            sum += k[j + 3] * raw[p < 0 ? 0 : p >= n ? n - 1 : p];
        }
        int32_t h = height((sum + 32) >> 6, floor, base, scale);

        if (raw[i] > floor + WF3D_SIG) {    /* a signal: its own level, a one-pixel CW peak at its height */
            int32_t p = height(raw[i], floor, base, scale);

            if (p > h) h = p;
        }
        out[i] = h;
    }
}

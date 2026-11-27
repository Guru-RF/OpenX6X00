/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/wf3d_shape.c: a row's levels to the 3D's trace heights. The noise around the floor smooth and
 *  low (at the base), below it pressed under the base, the signals above it at their height (a one-pixel CW peak
 *  kept), a faint signal clearly above the noise, the top clamped.
 *    wf3d_shape_test
 */

#include "wf3d_shape.h"

#include <stdio.h>
#include <string.h>

#define N       800
#define FLOOR   100
#define BASE    24

static int fails, checks;

static void check(int ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

static uint8_t raw[N], out[N];

static void noise(void) {
    for (int i = 0; i < N; i++) raw[i] = i & 1 ? 108 : 92;
}

static void min_max(int from, int to, int *lo, int *hi) {
    *lo = 255, *hi = 0;
    for (int i = from; i < to; i++) {
        if (out[i] < *lo) *lo = out[i];
        if (out[i] > *hi) *hi = out[i];
    }
}

static void flat_noise_is_smooth(void) {
    int lo, hi;

    noise();
    wf3d_shape(raw, N, FLOOR, BASE, 256, out);
    min_max(0, N, &lo, &hi);
    check(lo >= BASE - 6 && hi <= BASE + 6, "flat_noise_is_smooth: around the base", hi);
    check(hi - lo <= 6, "flat_noise_is_smooth: smooth", hi - lo);
}

static void noise_below_base(void) {
    int lo, hi;

    memset(raw, 60, N);
    wf3d_shape(raw, N, FLOOR, BASE, 256, out);
    min_max(0, N, &lo, &hi);
    check(hi <= BASE, "noise_below_base", hi);
}

static void floor_maps_to_base(void) {
    int lo, hi;

    memset(raw, FLOOR, N);
    wf3d_shape(raw, N, FLOOR, BASE, 256, out);
    min_max(0, N, &lo, &hi);
    check(lo >= BASE - 1 && hi <= BASE + 1, "floor_maps_to_base", lo);
}

static void cw_spike_kept(void) {
    memset(raw, FLOOR, N);
    raw[400] = 160;
    wf3d_shape(raw, N, FLOOR, BASE, 256, out);
    check(out[400] >= 77, "cw_spike_kept", out[400]);
}

static void faint_cw_above_noise(void) {
    int lo, hi;

    noise();
    raw[399] = raw[400] = raw[401] = 120;
    wf3d_shape(raw, N, FLOOR, BASE, 256, out);
    min_max(0, 380, &lo, &hi);
    check(out[400] >= hi + 10, "faint_cw_above_noise", out[400] - hi);
}

static void clamp_top(void) {
    int lo, hi;

    memset(raw, 255, N);
    wf3d_shape(raw, N, FLOOR, BASE, 1024, out);
    min_max(0, N, &lo, &hi);
    check(lo == 255, "clamp_top", lo);
}

int main(void) {
    flat_noise_is_smooth();
    noise_below_base();
    floor_maps_to_base();
    cw_spike_kept();
    faint_cw_above_noise();
    clamp_top();
    printf("wf3d_shape: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}

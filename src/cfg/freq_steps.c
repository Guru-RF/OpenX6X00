/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The tuning steps FST goes through (see freq_steps.h)
 */

#include "freq_steps.h"

const int32_t freq_steps[FREQ_STEPS_N] = {10, 100, 500, 1000, 5000, 10000, 25000, 100000};

static int held(int i) {
    return i < 0 ? 0 : i >= FREQ_STEPS_N ? FREQ_STEPS_N - 1 : i;
}

/* lo and hi held to the list and in order */
static void range(int *lo, int *hi) {
    int a = held(*lo), b = held(*hi);

    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

int32_t freq_steps_next(int32_t step, bool up, int lo, int hi) {
    int i = 0;
    int j;

    range(&lo, &hi);
    while (i < FREQ_STEPS_N && freq_steps[i] < step) i++;   /* the first at or above it */
    if (up) {
        j = i < FREQ_STEPS_N && freq_steps[i] == step ? i + 1 : i;
        if (j < lo || j > hi) j = lo;
    } else {
        j = i - 1;                                          /* the last below it */
        if (j < lo || j > hi) j = hi;
    }
    return freq_steps[j];
}

bool freq_steps_at_end(int32_t step, bool up, int lo, int hi) {
    range(&lo, &hi);
    return up ? step >= freq_steps[hi] : step <= freq_steps[lo];
}

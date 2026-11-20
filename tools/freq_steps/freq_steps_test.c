/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the tuning steps (src/cfg/freq_steps.c). The whole list (0 to 7, the default) goes as FST always went:
 *  up and down, round at both ends, a step not in the list to the one above or below it. A range (10 to 500 Hz) goes
 *  round within itself, a step outside it comes into it, the range's ends in either order. A knob stops at the ends.
 */

#include "freq_steps.h"

#include <stdio.h>

static int bad, cases;

static void next(int32_t step, bool up, int lo, int hi, int32_t want) {
    int32_t got = freq_steps_next(step, up, lo, hi);

    cases++;
    if (got != want) {
        printf("WRONG next(%d, %s, %d..%d) = %d, want %d\n", (int)step, up ? "up" : "down", lo, hi, (int)got,
               (int)want);
        bad++;
    }
}

static void end(int32_t step, bool up, int lo, int hi, bool want) {
    cases++;
    if (freq_steps_at_end(step, up, lo, hi) != want) {
        printf("WRONG at_end(%d, %s, %d..%d), want %d\n", (int)step, up ? "up" : "down", lo, hi, want);
        bad++;
    }
}

int main(void) {
    /* The whole list, as before: every step up and down, round at both ends */
    for (int i = 0; i < FREQ_STEPS_N; i++) {
        next(freq_steps[i], true, 0, 7, freq_steps[(i + 1) % FREQ_STEPS_N]);
        next(freq_steps[i], false, 0, 7, freq_steps[(i + FREQ_STEPS_N - 1) % FREQ_STEPS_N]);
    }
    /* Not in the list (the Panel's API): to the one above or below it, round past the ends */
    next(250, true, 0, 7, 500);
    next(250, false, 0, 7, 100);
    next(5, true, 0, 7, 10);
    next(5, false, 0, 7, 100000);
    next(200000, true, 0, 7, 10);
    next(200000, false, 0, 7, 100000);

    /* 10 to 500 Hz: 10, 100, 500 and round */
    next(10, true, 0, 2, 100);
    next(100, true, 0, 2, 500);
    next(500, true, 0, 2, 10);
    next(10, false, 0, 2, 500);
    next(500, false, 0, 2, 100);
    /* A step above the range (100 kHz from before it was narrowed): down to its top, up round to its bottom */
    next(100000, false, 0, 2, 500);
    next(100000, true, 0, 2, 10);
    next(1000, false, 0, 2, 500);
    next(1000, true, 0, 2, 10);
    /* 500 Hz to 10 kHz, a step below it: up to its bottom, down round to its top */
    next(10, true, 2, 5, 500);
    next(100, true, 2, 5, 500);
    next(100, false, 2, 5, 10000);
    next(10000, true, 2, 5, 500);
    /* The ends in either order, and out of the list's bounds */
    next(500, true, 2, 0, 10);
    next(10, false, 2, 0, 500);
    next(10, true, -3, 1, 100);
    next(100000, true, 6, 99, 25000);
    /* One step only: it stays */
    next(500, true, 2, 2, 500);
    next(500, false, 2, 2, 500);
    next(10, true, 2, 2, 500);

    /* A knob stops at the ends of the range */
    end(100000, true, 0, 7, true);
    end(25000, true, 0, 7, false);
    end(10, false, 0, 7, true);
    end(100, false, 0, 7, false);
    end(500, true, 0, 2, true);
    end(100, true, 0, 2, false);
    end(100000, true, 0, 2, true);
    end(10, false, 0, 2, true);
    end(500, false, 2, 5, true);
    end(1000, false, 2, 5, false);

    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}

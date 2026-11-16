/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of a knob press, short or held (src/knob_press.h, #282), as screen_main.c looks at it every
 *  20 ms: a quick press and one let go just before the hold time short, one held to it or past it held, each told
 *  once; a press with nothing down behind it (a keyboard's Enter) short at the first look; no answer without a press;
 *  the tick counter wrapping during a press. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "knob_press.h"

#include <stdio.h>

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

#define LOOK_MS 20  /* screen_main.c's timer */

/* A press at start, down for held ms, looked at every LOOK_MS from start on (the first look at once, as
 * screen_main.c does): what it came out as, and when (ms after start). Looks go on after the answer, to see it once */
static knob_press_t press(uint32_t start, uint32_t held, uint32_t *at) {
    knob_press_watch_t w = {0};
    knob_press_t       got = KNOB_PRESS_WAIT;
    int                answers = 0;

    knob_press_begin(&w, start);
    for (uint32_t t = 0; t <= 2000; t += LOOK_MS) {
        knob_press_t r = knob_press_look(&w, start + t, t < held);

        if (r != KNOB_PRESS_WAIT) {
            if (!answers++) {
                got = r;
                *at = t;
            }
        }
    }
    return answers == 1 ? got : KNOB_PRESS_WAIT;
}

int main(void) {
    uint32_t at = 0;

    check(press(1000, 80, &at) == KNOB_PRESS_SHORT && at == 80, "an 80 ms press: short, at the release");
    check(press(1000, 0, &at) == KNOB_PRESS_SHORT && at == 0, "nothing down behind it (Enter): short at once");
    check(press(1000, KNOB_PRESS_HOLD_MS - 20, &at) == KNOB_PRESS_SHORT, "let go just before the hold time: short");
    check(press(1000, KNOB_PRESS_HOLD_MS + LOOK_MS, &at) == KNOB_PRESS_HELD && at == KNOB_PRESS_HOLD_MS,
          "still down at the hold time: held, then");
    check(press(1000, 1300, &at) == KNOB_PRESS_HELD && at == KNOB_PRESS_HOLD_MS,
          "x6drive's long press (1.3 s): held at the hold time, nothing at the release");
    check(press(0xFFFFFFFFu - 100, 1300, &at) == KNOB_PRESS_HELD && at == KNOB_PRESS_HOLD_MS,
          "the tick counter wrapping during the press");
    check(press(0xFFFFFFFFu - 100, 80, &at) == KNOB_PRESS_SHORT && at == 80, "a short one across the wrap");

    knob_press_watch_t idle = {0};

    check(knob_press_look(&idle, 5000, true) == KNOB_PRESS_WAIT, "no press watched: no answer");
    check(knob_press_look(&idle, 5000, false) == KNOB_PRESS_WAIT, "no press watched, nothing down: no answer");

    /* A second press after the first was told: watched afresh */
    knob_press_watch_t w = {0};

    knob_press_begin(&w, 100);
    check(knob_press_look(&w, 120, false) == KNOB_PRESS_SHORT, "first press short");
    knob_press_begin(&w, 300);
    check(knob_press_look(&w, 320, true) == KNOB_PRESS_WAIT, "second press still down");
    check(knob_press_look(&w, 300 + KNOB_PRESS_HOLD_MS, true) == KNOB_PRESS_HELD, "second press held");

    printf("knob_press: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}

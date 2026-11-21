/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The tuning steps FST goes through (freq_steps.c)
 *
 *  FST, a keyboard's + and _ and a knob's Step control go through the steps from 10 Hz to 100 kHz. Settings > Display
 *  > MAIN SCREEN > Tuning steps narrows that to a range of them, lo to hi (indexes into freq_steps[]): 10 Hz to 500 Hz
 *  for an operator who never wants the big steps. FST goes round within the range, a knob stops at its ends.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREQ_STEPS_N 8

/* 10, 25 and 100 kHz for AM and FM on VHF and UHF and to cross a band quicker (#234) */
extern const int32_t freq_steps[FREQ_STEPS_N];

/* The step after step (up) or before it in freq_steps[lo..hi], round at both ends. lo and hi in either order, each
 * held to 0..FREQ_STEPS_N-1. A step not in the range (the Panel's API takes any, or the range was narrowed) goes to
 * the step of the range above or below it, round past the end */
int32_t freq_steps_next(int32_t step, bool up, int lo, int hi);

/* step is at the top (up) or bottom end of freq_steps[lo..hi], or past it: a knob turned that way stops */
bool freq_steps_at_end(int32_t step, bool up, int lo, int hi);

#ifdef __cplusplus
}
#endif

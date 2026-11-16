/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CB: the 27 MHz Citizens Band channels of a country's plan (cb.h) to choose and tune
 */

#pragma once

#include "cb.h"
#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_cb;

/* The plan the app shows (cb_plan_t): the one chosen with Plan, else the radio's region's. The Scanner's CB list too */
int dialog_cb_plan(void);

/* The mode chosen with Mode, CB_MODES none: the plan's own */
cb_mode_t dialog_cb_mode(void);

/* The radio's mode (radio_mode_t) for a CB mode */
int dialog_cb_radio_mode(cb_mode_t mode);

/* The main knob turned diff detents at old (screen_main.c): true when it stepped a channel of the plan instead of the
 * frequency, which happens only with the app open, Knob on channels and the radio inside the plan; true without a step
 * while the radio transmits there */
bool dialog_cb_knob(int32_t old, int16_t diff);

#ifdef __cplusplus
}
#endif

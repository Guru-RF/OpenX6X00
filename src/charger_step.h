/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Charger setting stepped by a key (#276)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

/* A key given "Charger Off/On/Shadow" in Settings > Keys takes Settings > General > CONTROLS > Charger to its next
 * state, in the order of the setting's list: Off, On, Shadow, and round to Off. The states are radio_charger_t's
 * (radio.h; screen_main.c checks they are these). Only the header, so the host test (tools/charger_step) takes it
 * alone. */

#pragma once

#define CHARGER_STEP_OFF    0
#define CHARGER_STEP_ON     1
#define CHARGER_STEP_SHADOW 2
#define CHARGER_STEPS       3

/* The state after now; a value that is none of them (a damaged setting) goes to Off */
static inline int charger_step_next(int now) {
    return now >= 0 && now < CHARGER_STEPS - 1 ? now + 1 : CHARGER_STEP_OFF;
}

/* The state as the setting's list names it */
static inline const char *charger_step_name(int state) {
    switch (state) {
        case CHARGER_STEP_ON:       return "On";
        case CHARGER_STEP_SHADOW:   return "Shadow";
        default:                    return "Off";
    }
}

/* The base is told to charge with On only: Shadow charges once the radio is switched off (radio_poweroff()) */
static inline int charger_step_charges(int state) {
    return state == CHARGER_STEP_ON;
}

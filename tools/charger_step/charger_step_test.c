/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the Charger setting stepped by a key (src/charger_step.h, #276): Off, On, Shadow and
 *  round to Off, three uses back where it started; a value that is no state goes to Off; the names are the setting's
 *  list's; the base is told to charge with On only. The setting, the base and the message are screen_main.c's: not
 *  run here. See the Makefile.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "charger_step.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int checks, failed;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

int main(void) {
    check(charger_step_next(CHARGER_STEP_OFF) == CHARGER_STEP_ON, "Off: On next");
    check(charger_step_next(CHARGER_STEP_ON) == CHARGER_STEP_SHADOW, "On: Shadow next");
    check(charger_step_next(CHARGER_STEP_SHADOW) == CHARGER_STEP_OFF, "Shadow: round to Off");
    for (int s = 0; s < CHARGER_STEPS; s++) {
        int n = s;

        for (int i = 0; i < CHARGER_STEPS; i++)
            n = charger_step_next(n);
        check(n == s, "three uses: back at the start, every state passed");
    }
    check(charger_step_next(3) == CHARGER_STEP_OFF && charger_step_next(255) == CHARGER_STEP_OFF &&
          charger_step_next(-1) == CHARGER_STEP_OFF, "a value that is no state: Off");

    /* As the row in Settings names them (" Off \n On \n Shadow") */
    check(!strcmp(charger_step_name(CHARGER_STEP_OFF), "Off"), "0 is Off");
    check(!strcmp(charger_step_name(CHARGER_STEP_ON), "On"), "1 is On");
    check(!strcmp(charger_step_name(CHARGER_STEP_SHADOW), "Shadow"), "2 is Shadow");

    check(!charger_step_charges(CHARGER_STEP_OFF), "Off: the base is not asked to charge");
    check(charger_step_charges(CHARGER_STEP_ON), "On: it is");
    check(!charger_step_charges(CHARGER_STEP_SHADOW), "Shadow: not while the radio is on");

    printf("charger_step: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}

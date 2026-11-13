/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - when the first-start guide comes up by itself, and its ESC (see first_start_rule.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "first_start_rule.h"

#include <ctype.h>
#include <stddef.h>

/* The web configurator takes any text for the callsign: one of spaces only is none, as the apps that need one see it */
static bool blank(const char *s) {
    if (!s) {
        return true;
    }
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return !*s;
}

bool first_start_due(const char *callsign, bool done, bool unsaved) {
    return !done && !unsaved && blank(callsign);
}

/* Unsigned: the tick goes round after 49 days, and the difference is still right across it */
bool first_start_esc(first_start_esc_t *esc, uint32_t now_ms) {
    if (esc->armed && (uint32_t)(now_ms - esc->at) <= FIRST_START_ESC_MS) {
        esc->armed = false;
        return true;
    }
    esc->armed = true;
    esc->at    = now_ms;
    return false;
}

void first_start_esc_reset(first_start_esc_t *esc) {
    esc->armed = false;
}

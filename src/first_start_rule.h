/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - when the first-start guide comes up by itself, and its ESC (first_start_rule.c)
 *
 *  The guide (first_start.c) comes up once, at the start of a radio with no callsign that has not had it: a new card,
 *  or the settings reset (Settings > Info > Factory reset deletes params.db, where the mark is kept). Never on a radio
 *  that has a callsign: one updated from an earlier version and in use gets no guide. Finishing it or skipping it as
 *  a whole sets the mark (the settings' first_start_done), and so does leaving it another way (GEN, another app): it
 *  does not come back by itself after that, Settings > General runs it again. A start that runs on the defaults
 *  without saving them (params.db not usable by this version, or not readable now: params/db.c) shows no guide: the
 *  radio's own settings are still on the card, and the mark could not be kept.
 *
 *  ESC asks first ("Skip the guide?"), as the other dialogs ask for what can't be taken back ("Press Delete again"):
 *  a second ESC within FIRST_START_ESC_MS skips the guide. Here only the decisions; first_start.c shows the guide.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FIRST_START_ESC_MS  4000    /* the second ESC within this skips the guide */

/* At the start: true when the guide comes up. callsign the station's ("" or spaces: none), done the mark, unsaved a
 * start on a copy of the defaults that saves nothing */
bool first_start_due(const char *callsign, bool done, bool unsaved);

/* ESC pressed at now_ms (LVGL's tick): true when it is the second within FIRST_START_ESC_MS, the guide then skipped;
 * false when it only asks. Anything else done in the guide calls first_start_esc_reset() */
typedef struct {
    bool     armed;     /* an ESC asked */
    uint32_t at;        /* when */
} first_start_esc_t;

bool first_start_esc(first_start_esc_t *esc, uint32_t now_ms);
void first_start_esc_reset(first_start_esc_t *esc);

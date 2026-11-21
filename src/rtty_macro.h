/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - RTTY's macros (#312)
 *
 *  The RTTY app's macro list, kept in /mnt/rtty_macros.txt, one macro per line as "name<TAB>text", as the PSK31 and
 *  Olivia lists (psk_macro.c, olivia_macro.c), the web configurator's Macros page edits it too. RTTY sends capitals
 *  only, so the starter set is in capitals. Read each time the app opens.
 *
 *  Storage only: the buttons and the editor are in rtty_send.c. The tokens (<MYCALL>, <CALL>, <RST>, <RSTR>) are
 *  filled in by psk_macro_expand(), from the station's callsign and the QSO state all the keyboard modes share
 *  (digi_qso.c).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTTY_MACRO_MAX       6      /* three on each of the two macro pages, next to Stop TX */
#define RTTY_MACRO_NAME_LEN  24
#define RTTY_MACRO_TEXT_LEN  200

typedef struct {
    char name[RTTY_MACRO_NAME_LEN];
    char text[RTTY_MACRO_TEXT_LEN];
} rtty_macro_t;

/* Load the list; a missing file is written with the starter set. A file that is there is taken as it is, empty too */
void                rtty_macros_load(void);
bool                rtty_macros_save(void);

int                 rtty_macros_count(void);
const rtty_macro_t *rtty_macro_get(int idx);

/* add gives the new index, -1 when all the slots are taken. Tabs and line breaks become spaces: they would break the
 * file. None of them saves: rtty_macros_save() */
int                 rtty_macro_add(const char *name, const char *text);
void                rtty_macro_set(int idx, const char *name, const char *text);
void                rtty_macro_delete(int idx);

#ifdef __cplusplus
}
#endif

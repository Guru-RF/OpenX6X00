/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a last good copy of the settings database (params/db.c keeps one of params.db).
 *
 *  The copy is made with SQLite's backup API from a connection of its own: what was committed at that moment, whole,
 *  whatever the GUI's threads write meanwhile (they see their own transaction, a copy taken through their connection
 *  would take a half-written one with it). It is one file in rollback mode (a copy of a WAL database is WAL too, and
 *  would want its -wal beside it), checked with PRAGMA quick_check, on the card (fsync) and renamed over the copy
 *  before it only when sound: a copy of a file that went bad never replaces a good one.
 */

#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* from (an open database or not) copied to `to` through to.tmp. False: `to` is left as it was */
bool db_keep_copy(const char *from, const char *to);

/* The file is there and sound: it opens, PRAGMA quick_check says ok, and it is the GUI's own (its version row, 1 or
 * more). *when, if not NULL: when it was made */
bool db_keep_sound(const char *path, time_t *when);

#ifdef __cplusplus
}
#endif

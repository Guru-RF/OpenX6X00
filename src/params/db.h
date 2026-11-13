/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#pragma once

#include <stdbool.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>

extern sqlite3          *db;

/* params.db opened, repaired and migrated, cfg_db_init done; one the GUI cannot use is put aside for its last good
 * copy (database_keep) or the defaults (db.c). False: no settings database at all */
bool database_init();

/* What to say on the screen once it is up when the settings did not come from params.db as it was (the last good
 * copy, the defaults); NULL when they did */
const char *database_note(void);

/* The last good copy of params.db (params/db_keep.h), made now in the caller's thread: of a start whose settings came
 * from params.db and loaded, never of the defaults in /tmp. False: none made, the one before kept. A clean power-off
 * (radio.c), after the settings' last writes */
bool database_keep(void);

/* The same in a thread of its own: once the GUI has run a minute (gui_main.c) */
void database_keep_later(void);

/* The copy gone: a factory reset, whose next start would otherwise not be on the defaults after a damaged file */
void database_keep_drop(void);

/* True when params.db is the GUI's own and this version could not migrate or use it (left as it is, this start on
 * the defaults): a new version is then not confirmed, and x6100-update's boot-check takes it back after 3 starts */
bool database_version_fault(void);

/* True when this start runs on a copy of the defaults in /tmp (params.db not usable by this version, or not readable
 * now): nothing set is kept, and the radio's own settings are still on the card */
bool database_unsaved(void);

bool sql_query_exec(const char *sql);

/* A save: begin, the writes (false: that one failed, its dirty flag stays), commit (false:
 * nothing of it is saved, or not all; every dirty flag that is not saved stays set) */
void params_write_begin(void);
bool params_write_int(const char *name, int data, bool *dirty);
bool params_write_float(const char *name, float data, bool *dirty);
bool params_write_text(const char *name, const char *data, bool *dirty);
bool params_write_commit(void);

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

/* params.db opened, repaired and migrated, cfg_db_init done; one the GUI cannot use is put aside for the defaults
 * (db.c). False: no settings database at all */
bool database_init();

/* What to say on the screen once it is up when the settings did not come from params.db; NULL when they did */
const char *database_note(void);

/* True when params.db is the GUI's own and this version could not migrate or use it (left as it is, this start on
 * the defaults): a new version is then not confirmed, and x6100-update's boot-check takes it back after 3 starts */
bool database_version_fault(void);

bool sql_query_exec(const char *sql);

/* A save: begin, the writes (false: that one failed, its dirty flag stays), commit (false:
 * nothing of it is saved, or not all; every dirty flag that is not saved stays set) */
void params_write_begin(void);
bool params_write_int(const char *name, int data, bool *dirty);
bool params_write_float(const char *name, float data, bool *dirty);
bool params_write_text(const char *name, const char *data, bool *dirty);
bool params_write_commit(void);

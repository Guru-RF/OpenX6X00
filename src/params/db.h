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

bool database_init();

bool sql_query_exec(const char *sql);

/* A save: begin, the writes (false: that one failed, its dirty flag stays), commit (false:
 * nothing of it is saved, or not all; every dirty flag that is not saved stays set) */
void params_write_begin(void);
bool params_write_int(const char *name, int data, bool *dirty);
bool params_write_float(const char *name, float data, bool *dirty);
bool params_write_text(const char *name, const char *data, bool *dirty);
bool params_write_commit(void);

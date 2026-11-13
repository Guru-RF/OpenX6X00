/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Stand-ins for what src/params/db.c calls outside src/params (tools/settings_keep): the new settings' tables
 *  (src/cfg, whose statements the test does not need), the start trace (its last line kept for the test) and LVGL's
 *  log (the test prints its own).
 */

#include <lvgl/src/misc/lv_log.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

extern char start_trace_last[512];

bool cfg_db_init(sqlite3 *database) {
    (void)database;
    return true;
}

void cfg_db_shutdown(void) {
}

void cfg_db_tx_lock(void) {
}

void cfg_db_tx_unlock(void) {
}

bool cfg_db_in_transaction(void) {
    return false;
}

void start_trace(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(start_trace_last, sizeof(start_trace_last), fmt, ap);
    va_end(ap);
}

void _lv_log_add(lv_log_level_t level, const char *file, int line, const char *func, const char *format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)func;
    (void)format;
}

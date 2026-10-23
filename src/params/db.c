/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#include "db.h"
#include "../board.h"
#include "../cfg/db.h"

#include "migrations.h"
#include "../util.h"

#include <string.h>
#include <lvgl/src/misc/lv_log.h>


sqlite3                 *db = NULL;

static sqlite3_stmt     *insert_stmt;


/* A params.db with no usable bands (an old, foreign or broken one kept on the DATA partition
 * when the SD card was written again: the tables missing, empty or with other columns): both
 * band tables made again from the default database, the other settings kept */
#define DEFAULT_DB BOARD_SHARE "/params.default.db"

/* The query works and, with need_row, returns a row */
static bool query_ok(const char *sql, bool need_row) {
    sqlite3_stmt *st;
    bool          ok;

    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) {
        return false;
    }
    ok = !need_row || sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return ok;
}

/* A damaged params.db (e.g. the radio switched off while writing): indexes that miss rows
 * make every query through them fail. They are made again from the tables (duplicates of
 * a UNIQUE ... ON CONFLICT REPLACE index dropped), then the file is compacted. */
static void db_repair(void) {
    sqlite3_stmt *st;
    bool          ok = false;

    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check(1)", -1, &st, 0) == SQLITE_OK) {
        ok = sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "ok");
        sqlite3_finalize(st);
    }
    if (ok) {
        return;
    }
    LV_LOG_WARN("params.db damaged: rebuilding its indexes");
    if (sqlite3_exec(db, "REINDEX; VACUUM;", NULL, NULL, NULL) != SQLITE_OK) {
        LV_LOG_ERROR("Can't repair params.db: %s", sqlite3_errmsg(db));
    }
}

static void bands_check(void) {
    if (query_ok("SELECT id, name, start_freq, stop_freq, type FROM main.bands", true) &&
        query_ok("SELECT bands_id, name, val FROM main.band_params", false)) {
        return;
    }
    LV_LOG_WARN("params.db has no usable bands: loading the default ones");
    if (sqlite3_exec(db,
            "ATTACH '" DEFAULT_DB "' AS def;"
            "BEGIN;"
            "DROP TABLE IF EXISTS main.bands;"
            "DROP TABLE IF EXISTS main.band_params;"
            "CREATE TABLE main.bands(id INTEGER PRIMARY KEY, name TEXT, start_freq INTEGER, "
                "stop_freq INTEGER, type INTEGER);"
            "CREATE TABLE main.band_params(bands_id INTEGER, name TEXT, val INTEGER, "
                "UNIQUE (bands_id, name) ON CONFLICT REPLACE);"
            "INSERT INTO main.bands SELECT id, name, start_freq, stop_freq, type FROM def.bands;"
            "INSERT INTO main.band_params SELECT bands_id, name, val FROM def.band_params;"
            "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        LV_LOG_ERROR("Can't load the default bands: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_exec(db, "DETACH def;", NULL, NULL, NULL);
}

static void errorLogCallback(void *pArg, int iErrCode, const char *zMsg){
    LV_LOG_ERROR("(%d) %s\n", iErrCode, zMsg);
}

bool database_init() {
    sqlite3_config(SQLITE_CONFIG_LOG, errorLogCallback, NULL);
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);

    int rc = sqlite3_open(BOARD_DATA "/params.db", &db);

    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Can't open params.db");
        return false;
    }

    db_repair();
    rc = migrations_apply();
    if (rc != 0) {
        return false;
    }
    bands_check();

    // Some optimizations
    //
    sqlite3_exec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size = -4096;", NULL, NULL, NULL);


    rc = sqlite3_prepare_v2(db, "INSERT INTO params(name, val) VALUES(?, ?)", -1, &insert_stmt, 0);

    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("Can't prepare insert statement for params");
        return false;
    }
    return true;
}

bool sql_query_exec(const char *sql) {
    char    *err = 0;
    int     rc;

    rc = sqlite3_exec(db, sql, NULL, NULL, &err);

    if (rc != SQLITE_OK) {
        LV_LOG_ERROR(err);
        return false;
    }
    return true;
}


/* The legacy settings' writes, between params_write_begin() and params_write_commit(). A
 * setting's dirty flag is cleared only once the transaction holding its row has committed:
 * a failed step or commit (a full card, an I/O error) leaves it for the next save (audit A14) */
#define PENDING_MAX 256
static bool *pending[PENDING_MAX];
static int  npending;
static bool pending_short;  /* a write failed, or did not fit the list: the save is not complete */

void params_write_begin(void) {
    npending = 0;
    pending_short = false;
    cfg_db_tx_lock();       /* the new settings' flush waits: its rows would be ours to roll back (audit B12) */
    sql_query_exec("BEGIN");
}

static bool write_step(bool *dirty) {
    int rc = sqlite3_step(insert_stmt);

    sqlite3_reset(insert_stmt);
    sqlite3_clear_bindings(insert_stmt);
    if (rc != SQLITE_DONE) {
        LV_LOG_ERROR("Settings write: %s", sqlite3_errmsg(db));
        pending_short = true;   /* stays dirty: written again next time */
        return false;
    }
    if (npending < PENDING_MAX)
        pending[npending++] = dirty;
    else
        pending_short = true;
    return true;
}

bool params_write_int(const char *name, int data, bool *dirty) {
    sqlite3_bind_text(insert_stmt, 1, name, strlen(name), 0);
    sqlite3_bind_int(insert_stmt, 2, data);
    return write_step(dirty);
}

bool params_write_float(const char *name, float data, bool *dirty) {
    sqlite3_bind_text(insert_stmt, 1, name, strlen(name), 0);
    sqlite3_bind_double(insert_stmt, 2, (double) data);
    return write_step(dirty);
}

bool params_write_text(const char *name, const char *data, bool *dirty) {
    sqlite3_bind_text(insert_stmt, 1, name, strlen(name), 0);
    sqlite3_bind_text(insert_stmt, 2, data, strlen(data), 0);
    return write_step(dirty);
}

bool params_write_commit(void) {
    if (!sql_query_exec("COMMIT")) {
        sql_query_exec("ROLLBACK");
        cfg_db_tx_unlock();
        npending = 0;
        return false;
    }
    cfg_db_tx_unlock();
    for (int i = 0; i < npending; i++)
        *pending[i] = false;
    npending = 0;
    return !pending_short;
}

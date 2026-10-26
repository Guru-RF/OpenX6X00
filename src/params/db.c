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
#include "../start_trace.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <lvgl/src/misc/lv_log.h>


sqlite3                 *db = NULL;

static sqlite3_stmt     *insert_stmt;

#define PARAMS_DB   BOARD_DATA "/params.db"
#define SPARE_DB    "/tmp/x6100-params.db"      /* a start on the defaults that writes nothing on the card */

static char note[96];   /* for the screen once it is up: the settings did not come from params.db */
static bool version_fault;  /* the GUI's own params.db, which this version could not migrate or use */


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

/* PRAGMA integrity_check: SQLITE_OK when the file is sound, SQLITE_CORRUPT when it is not, or the code of what kept
 * the check from running (not a database, busy, an I/O error) */
static int integrity(void) {
    sqlite3_stmt *st;
    const char   *res;
    int           rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check(1)", -1, &st, 0);

    if (rc != SQLITE_OK) {
        return rc;
    }
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        res = (const char *)sqlite3_column_text(st, 0);
        rc = res && !strcmp(res, "ok") ? SQLITE_OK : SQLITE_CORRUPT;
    }
    sqlite3_finalize(st);
    return rc;
}

/* A damaged params.db (e.g. the radio switched off while writing): indexes that miss rows
 * make every query through them fail. They are made again from the tables (duplicates of
 * a UNIQUE ... ON CONFLICT REPLACE index dropped), then the file is compacted. What is still
 * damaged after that (a table's own page) is SQLITE_CORRUPT: it passed the migrations and the
 * statements, and the first read of the settings crashed the GUI at every start (#17) */
static int db_repair(void) {
    int rc = integrity();

    if (rc != SQLITE_CORRUPT) {
        return rc;
    }
    LV_LOG_WARN("params.db damaged: rebuilding its indexes");
    if (sqlite3_exec(db, "REINDEX; VACUUM;", NULL, NULL, NULL) != SQLITE_OK) {
        LV_LOG_ERROR("Can't repair params.db: %s", sqlite3_errmsg(db));
    }
    return integrity();
}

/* The file is the GUI's own: the version row its migrations keep, 1 or more (params.sql starts at 1). An empty or a
 * foreign file has none */
static bool db_ours(void) {
    sqlite3_stmt *st;
    bool          ours = false;

    if (sqlite3_prepare_v2(db, "SELECT id FROM version", -1, &st, 0) == SQLITE_OK) {
        ours = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) >= 1;
        sqlite3_finalize(st);
    }
    return ours;
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

/* One try at a settings database: open, repair, migrate, and the statements of both settings APIs. False with *err
 * SQLite's code for what failed, and *ours whether the file is the GUI's own (db_ours, before the migrations);
 * database_close() then */
static bool database_try(const char *path, int *err, bool *ours) {
    int rc = sqlite3_open(path, &db);

    *ours = false;
    if (rc != SQLITE_OK) {
        *err = rc;
        return false;
    }
    /* The web server may hold it for a moment when the GUI restarts: waited for during the start only */
    sqlite3_busy_timeout(db, 5000);

    rc = db_repair();
    if (rc != SQLITE_OK) {
        *err = rc;
        return false;
    }
    *ours = db_ours();
    if (migrations_apply() != 0) {
        *err = sqlite3_errcode(db);
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
        *err = rc;
        return false;
    }
    if (!cfg_db_init(db)) {
        *err = sqlite3_errcode(db);
        return false;
    }
    sqlite3_busy_timeout(db, 0);
    return true;
}

static void database_close(void) {
    cfg_db_shutdown();
    if (insert_stmt) {
        sqlite3_finalize(insert_stmt);
        insert_stmt = NULL;
    }
    sqlite3_close_v2(db);
    db = NULL;
}

/* SQLite's codes for a file that is itself damaged: what the integrity check found, or not a database at all */
static bool file_damaged(int err) {
    return (err & 0xff) == SQLITE_CORRUPT || (err & 0xff) == SQLITE_NOTADB;
}

/* SQLite's codes for SQL that did not fit the file (a migration's, or a statement's on a table missing or of another
 * shape). The others (busy, read-only, I/O, a full card) are the card's or the moment's */
static bool file_misfit(int err) {
    switch (err & 0xff) {
        case SQLITE_ERROR:
        case SQLITE_SCHEMA:
        case SQLITE_MISMATCH:
        case SQLITE_CONSTRAINT:
        case SQLITE_FORMAT:
        case SQLITE_RANGE:
        case SQLITE_TOOBIG:
            return true;
        default:
            return false;
    }
}

/* from onto to, through to.tmp: a power cut leaves either */
static bool copy_file(const char *from, const char *to) {
    char   tmp[PATH_MAX], buf[8192];
    FILE  *in = fopen(from, "rb"), *out;
    size_t n;
    bool   ok = in != NULL;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", to) >= (int)sizeof(tmp) || !ok || !(out = fopen(tmp, "wb"))) {
        if (in) fclose(in);
        return false;
    }
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        ok = fwrite(buf, 1, n, out) == n;
    }
    ok = ok && !ferror(in) && fflush(out) == 0 && fsync(fileno(out)) == 0;
    fclose(in);
    ok = fclose(out) == 0 && ok && rename(tmp, to) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

/* The card's params.db and its journals put aside under one name (opened together they are the file as it was: a
 * journal left beside a new params.db would be played into it), and the defaults in its place */
static bool put_aside(char *aside, size_t size) {
    static const char *const side[] = { "", "-wal", "-shm", "-journal" };
    char       from[PATH_MAX], to[PATH_MAX], when[24];
    time_t     now = time(NULL);
    struct tm  tm;

    gmtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y%m%d-%H%M%SZ", &tm);
    if (snprintf(aside, size, BOARD_DATA "/params-unusable-%s.db", when) >= (int)size) {
        return false;
    }
    for (size_t i = 0; i < sizeof(side) / sizeof(side[0]); i++) {
        if (snprintf(from, sizeof(from), PARAMS_DB "%s", side[i]) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s%s", aside, side[i]) >= (int)sizeof(to)) {
            return false;
        }
        if (rename(from, to) != 0 && errno != ENOENT) {
            if (i == 0) {
                return false;   /* the card does not take it: params.db stays as it is */
            }
            unlink(from);
        }
    }
    return copy_file(DEFAULT_DB, PARAMS_DB);
}

/* The settings database: params.db on the card. One the GUI cannot use stopped it at every start (a setting left
 * NULL, or cfg_db_init's exit), and x6100_daemon started it again for ever behind the boot logo (#17): a damaged
 * file, and an empty or foreign one that does not fit, is put aside and the defaults take its place. One of the
 * GUI's own that this version cannot migrate or use is not put aside: the fault is the version's, and an update
 * goes back to the one before (database_version_fault). That start, and one whose card does not let the file be
 * replaced or opened (read-only, busy, I/O errors), runs on a copy of the defaults in /tmp. Each is said on the
 * screen (database_note) and in the start trace */
bool database_init() {
    static const char *const side[] = { "-wal", "-shm", "-journal" };
    int         err = SQLITE_OK, first;
    bool        ours;
    char        aside[PATH_MAX] = "";
    const char *why = "not usable now";

    sqlite3_config(SQLITE_CONFIG_LOG, errorLogCallback, NULL);
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);

    if (database_try(PARAMS_DB, &err, &ours)) {
        return true;
    }
    database_close();
    first = err;
    fprintf(stderr, "params.db: %s (%d)%s\n", sqlite3_errstr(first), first, ours ? ", the GUI's own" : "");
    if (file_damaged(first) || (file_misfit(first) && !ours)) {
        why = "unusable and not replaced";
        if (put_aside(aside, sizeof(aside))) {
            if (database_try(PARAMS_DB, &err, &ours)) {
                fprintf(stderr, "params.db: put aside as %s, the defaults in its place\n", aside);
                start_trace("settings: params.db unusable (%s), put aside as %s; the defaults loaded",
                            sqlite3_errstr(first), strrchr(aside, '/') + 1);
                snprintf(note, sizeof(note), "Settings file unusable: put aside, defaults loaded");
                return true;
            }
            database_close();
            why = "unusable, put aside, and the defaults not usable there";
        }
    } else if (file_misfit(first)) {
        why = "not usable by this version";
        version_fault = true;
    }
    /* A copy of its own each start: a WAL the last such start left (a GUI killed before its checkpoint) would be
     * played into it, its settings back or the file malformed */
    for (size_t i = 0; i < sizeof(side) / sizeof(side[0]); i++) {
        char path[sizeof(SPARE_DB) + 16];

        snprintf(path, sizeof(path), SPARE_DB "%s", side[i]);
        unlink(path);
    }
    if (copy_file(DEFAULT_DB, SPARE_DB) && database_try(SPARE_DB, &err, &ours)) {
        fprintf(stderr, "params.db: this start runs on the defaults, " SPARE_DB "\n");
        start_trace("settings: params.db %s (%s); this start runs on the defaults, nothing saved", why,
                    sqlite3_errstr(first));
        snprintf(note, sizeof(note), version_fault ? "Settings not usable by this version: defaults, not saved"
                                                   : "Settings not readable: defaults, not saved");
        return true;
    }
    database_close();
    start_trace("settings: no settings database (%s, the defaults: %s)", sqlite3_errstr(first), sqlite3_errstr(err));
    return false;
}

const char *database_note(void) {
    return note[0] ? note : NULL;
}

bool database_version_fault(void) {
    return version_fault;
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

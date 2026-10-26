/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - the QSO log in SQLite (qso_log.h)
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "qso_log.h"
#include "board.h"

#include "util.h"
#include "msg.h"
#include "adif.h"
#include "params/params.h"

#include <lvgl/src/misc/lv_log.h>
#include <sqlite3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_DB  BOARD_DATA "/qso_log.db"

/* The table and its indexes as radios already have them on disk: never change a column */
static const char *schema[] = {
    "CREATE TABLE IF NOT EXISTS qso_log( "
        "ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
        "freq REAL CHECK ( freq > 0 ), "
        "band INT NOT NULL, "
        "mode INT NOT NULL, "
        "local_callsign TEXT NOT NULL, "
        "remote_callsign TEXT NOT NULL, "
        "canonized_remote_callsign TEXT NOT NULL, "
        "rsts INTEGER NOT NULL, "
        "rstr INTEGER NOT NULL, "
        "local_qth TEXT, "
        "remote_qth TEXT, "
        "local_grid TEXT, "
        "remote_grid TEXT, "
        "op_name TEXT, "
        "comment TEXT "
    ")",
    "CREATE INDEX IF NOT EXISTS qso_log_idx_canonized_remote_callsign "
        "ON qso_log(canonized_remote_callsign COLLATE NOCASE)",
    "CREATE INDEX IF NOT EXISTS qso_log_idx_mode ON qso_log(mode)",
    "CREATE INDEX IF NOT EXISTS qso_log_idx_ts ON qso_log(ts)",
    /* the same call at the same second is one QSO: a second insert is ignored */
    "CREATE UNIQUE INDEX IF NOT EXISTS qso_log_idx_ts_call ON qso_log(ts, remote_callsign)",
};

static sqlite3      *db;
static sqlite3_stmt *worked_stmt;

/* The bands as the log stores them, edges in kHz */
static const struct { uint32_t lo, hi; qso_log_band_t band; } bands[] = {
    {1800, 2000, BAND_160M},    {3500, 4000, BAND_80M},     {5351, 5367, BAND_60M},
    {7000, 7300, BAND_40M},     {10100, 10150, BAND_30M},   {14000, 14350, BAND_20M},
    {18068, 18168, BAND_17M},   {21000, 21450, BAND_15M},   {24890, 24990, BAND_12M},
    {28000, 29700, BAND_10M},   {50000, 54000, BAND_6M},
};

bool qso_log_init() {
    if (sqlite3_open(LOG_DB, &db) != SQLITE_OK) {
        LV_LOG_ERROR("Can't open " LOG_DB);
        return false;
    }
    for (size_t i = 0; i < sizeof(schema) / sizeof(schema[0]); i++) {
        char *err = NULL;

        if (sqlite3_exec(db, schema[i], NULL, NULL, &err) != SQLITE_OK) {
            LV_LOG_ERROR("%s", err);
            sqlite3_free(err);
            return false;
        }
    }
    return true;
}

qso_log_band_t qso_log_freq_to_band(uint64_t freq_hz) {
    uint32_t khz = freq_hz / 1000;      /* 32 bits, as the log always took it */

    for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++)
        if (khz >= bands[i].lo && khz <= bands[i].hi) return bands[i].band;
    return BAND_OTHER;
}

static void copy_field(char *dst, size_t size, const char *src) {
    if (src) snprintf(dst, size, "%s", src);
}

qso_log_record_t qso_log_record_create(const char *local_call, const char *remote_call,
                                       time_t qso_time, qso_log_mode_t mode, int rsts, int rstr, uint64_t freq_hz,
                                       const char *name, const char *qth,
                                       const char *local_grid, const char *remote_grid)
{
    qso_log_record_t r = {
        .time = qso_time,
        .mode = mode,
        .rsts = rsts,
        .rstr = rstr,
        .freq_mhz = (float) freq_hz / 1000000,
        .band = qso_log_freq_to_band(freq_hz),
    };

    copy_field(r.local_call, sizeof(r.local_call), local_call);
    copy_field(r.remote_call, sizeof(r.remote_call), remote_call);
    copy_field(r.name, sizeof(r.name), name);
    copy_field(r.qth, sizeof(r.qth), qth);
    copy_field(r.local_grid, sizeof(r.local_grid), local_grid);
    copy_field(r.remote_grid, sizeof(r.remote_grid), remote_grid);
    return r;
}

/* 1 stored, 0 already in the log (or refused by the table), -1 error */
int qso_log_record_save(qso_log_record_t qso) {
    sqlite3_stmt    *st;
    char            *canon;
    int             res = -1;

    if (!qso.local_call[0] || !qso.remote_call[0]) {
        LV_LOG_ERROR("QSO without a callsign");
        return -1;
    }
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO qso_log (ts, freq, band, mode, local_callsign, remote_callsign, rsts, rstr, "
            "local_grid, remote_grid, op_name, canonized_remote_callsign) "
            "VALUES (datetime(?1, 'unixepoch'), ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
            -1, &st, NULL) != SQLITE_OK)
    {
        LV_LOG_ERROR("QSO insert: %s", sqlite3_errmsg(db));
        return -1;
    }
    canon = util_canonize_callsign(qso.remote_call, true);

    /* The strings are bound as they are, empty ones too: an empty grid is stored as '' */
    if (sqlite3_bind_int64(st, 1, qso.time) == SQLITE_OK &&
        sqlite3_bind_double(st, 2, qso.freq_mhz) == SQLITE_OK &&
        sqlite3_bind_int(st, 3, qso.band) == SQLITE_OK &&
        sqlite3_bind_int(st, 4, qso.mode) == SQLITE_OK &&
        sqlite3_bind_text(st, 5, qso.local_call, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(st, 6, qso.remote_call, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_int(st, 7, qso.rsts) == SQLITE_OK &&
        sqlite3_bind_int(st, 8, qso.rstr) == SQLITE_OK &&
        sqlite3_bind_text(st, 9, qso.local_grid, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(st, 10, qso.remote_grid, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(st, 11, qso.name, -1, SQLITE_STATIC) == SQLITE_OK &&
        (canon ? sqlite3_bind_text(st, 12, canon, -1, SQLITE_STATIC) : sqlite3_bind_null(st, 12)) == SQLITE_OK)
    {
        if (sqlite3_step(st) == SQLITE_DONE)
            res = sqlite3_changes(db);
        else
            LV_LOG_ERROR("QSO insert: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    free(canon);
    return res;
}

qso_log_search_worked_t qso_log_search_worked(const char *callsign, qso_log_mode_t mode, qso_log_band_t band) {
    qso_log_search_worked_t res = SEARCH_WORKED_NO;
    char                    *canon;

    if (!worked_stmt) {
        if (sqlite3_prepare_v3(db, "SELECT DISTINCT band, mode FROM qso_log WHERE canonized_remote_callsign LIKE ?",
                               -1, SQLITE_PREPARE_PERSISTENT, &worked_stmt, NULL) != SQLITE_OK)
            return -1;
    } else {
        sqlite3_reset(worked_stmt);
    }
    canon = util_canonize_callsign(callsign, true);
    if (!canon) canon = strdup(callsign);
    if (sqlite3_bind_text(worked_stmt, 1, canon, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        free(canon);
        return -1;
    }
    free(canon);

    /* LIKE: any case; a % or _ in the call works as a wildcard */
    while (sqlite3_step(worked_stmt) == SQLITE_ROW) {
        res = SEARCH_WORKED_YES;
        if (sqlite3_column_int(worked_stmt, 0) == band && sqlite3_column_int(worked_stmt, 1) == mode) {
            res = SEARCH_WORKED_SAME_MODE;
            break;
        }
    }
    return res;
}

/* The radio's own callsign (Settings > STATION) as the import starts, taken on the GUI thread:
 * the one of a record that names neither OPERATOR nor STATION_CALLSIGN */
static char own_call[sizeof(params.callsign.x)];

/* The records of the file into the log. The file is retired to .bak once every record got
 * its answer from the database: added, already there, or refused for lacking a callsign
 * (no CALL, or no own call in the record nor on the radio: that one is final). A record the
 * database could not take (an error, a full card) keeps the file for the next start: the log
 * ignores a duplicate, so nothing is imported twice.
 * A file with no record in it is kept as .failed, so the operator sees it was not read
 * (audit A15) */
static void *import_thread(void *arg) {
    const char          *path = arg;
    qso_log_record_t    *recs = NULL;
    int                 total, unreadable = 0;
    unsigned int        added = 0, dup = 0, nocall = 0, failed = 0;
    char                to[128];

    pthread_detach(pthread_self());
    total = adif_read(path, &recs, &unreadable);
    if (total < 0) {
        msg_update_text_fmt("Import: %s could not be read", path);
        return NULL;
    }

    for (int i = 0; i < total; i++) {
        int n;

        if (!recs[i].local_call[0]) {
            snprintf(recs[i].local_call, sizeof(recs[i].local_call), "%s", own_call);
        }
        if (!recs[i].local_call[0] || !recs[i].remote_call[0]) {
            nocall++;
            continue;
        }
        n = qso_log_record_save(recs[i]);
        if (n > 0) added += n;
        else if (n == 0) dup++;
        else failed++;
        if ((i + 1) % 10 == 0) msg_update_text_fmt("Importing QSO: %d/%d", i + 1, total);
    }
    free(recs);

    if (total == 0) {
        snprintf(to, sizeof(to), "%s.failed", path);
        rename(path, to);
        if (unreadable)
            msg_update_text_fmt("No QSO readable in the ADIF file (%d broken): kept as %s", unreadable, to);
        else
            msg_update_text_fmt("No QSOs found in the ADIF file: kept as %s", to);
    } else if (failed) {
        msg_update_text_fmt("Imported %u QSOs, %u not saved: the file is kept for the next start", added, failed);
    } else {
        snprintf(to, sizeof(to), "%s.bak", path);
        rename(path, to);
        if (dup || nocall || unreadable)
            msg_update_text_fmt("Imported %u QSOs (%u already in the log, %u without a callsign, %d broken)",
                                added, dup, nocall, unreadable);
        else
            msg_update_text_fmt("Imported %u QSOs", added);
    }
    return NULL;
}

void qso_log_import_adif(const char *path) {
    pthread_t thr;

    if (access(path, F_OK) != 0) {
        LV_LOG_USER("No ADI file to import");
        return;
    }
    snprintf(own_call, sizeof(own_call), "%s", params.callsign.x);
    if (pthread_create(&thr, NULL, import_thread, (void *)path) != 0)
        LV_LOG_ERROR("ADIF import thread not started");
}

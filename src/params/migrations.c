/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */

#define _GNU_SOURCE

#include "migrations.h"

#include "db.h"
#include "../cfg/digital_modes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZEOF_ARRAY(arr) (sizeof(arr) > 0 ? sizeof(arr) / sizeof(arr[0]) : 0)

static sqlite3_stmt *update_ver_stmt;

static int get_current_version(int * ver);
static int set_current_version(int ver);

/* Migrations functions */
static int _0_init_migrations() {
    int rc;
    rc = sqlite3_exec(db, "INSERT INTO version(id) VALUES(0)", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot insert 0 version: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

static int _1_create_ftx_table() {
    int rc;
    char *query;
    rc = asprintf(&query,
        "BEGIN;"
        "CREATE TABLE IF NOT EXISTS digital_modes("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "label varchar(64) NOT NULL, "
            "freq INTEGER NOT NULL CHECK(freq > 0), "
            "mode INTEGER NOT NULL DEFAULT 3 CHECK(mode >= 0 AND mode <= 7), "
            "type INTEGER NOT NULL, "
            "CONSTRAINT freq_type_uniq UNIQUE(freq, type) "
        ");"
        "CREATE INDEX IF NOT EXISTS digital_modes_type_idx ON digital_modes (type);"
        "CREATE INDEX IF NOT EXISTS digital_modes_freq_idx ON digital_modes (freq);"
        "INSERT INTO digital_modes(label, freq, mode, type) "
        "SELECT m1.val, m2.val, m3.val, %u  "
        "FROM memory AS m1  "
        "INNER JOIN memory AS m2 ON m1.id == m2.id  "
        "INNER JOIN memory AS m3 ON m1.id == m3.id  "
        "WHERE m1.name='label' AND m2.name='vfoa_freq' AND m3.name = 'vfoa_mode' AND m1.id >= 100 AND m1.id < 200; "
        "INSERT INTO digital_modes(label, freq, mode, type) "
        "SELECT m1.val, m2.val, m3.val, %u  "
        "FROM memory AS m1  "
        "INNER JOIN memory AS m2 ON m1.id == m2.id  "
        "INNER JOIN memory AS m3 ON m1.id == m3.id  "
        "WHERE m1.name='label' AND m2.name='vfoa_freq' AND m3.name = 'vfoa_mode' AND m1.id >= 200; "
        "COMMIT",
        CFG_DIG_TYPE_FT8, CFG_DIG_TYPE_FT4
    );
    if (rc == -1) {
        printf("Cannot allocate SQL query\n");
        return 1;
    }
    rc = sqlite3_exec(db, query, NULL, NULL, NULL);
    free(query);
    if (rc != SQLITE_OK) {
        printf("Cannot migrate: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

static int _2_update_atu_freq() {
    int rc;
    rc = sqlite3_exec(db, "UPDATE atu SET freq=freq * 50000 + 25000 WHERE freq < 500000", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot update ATU frequencies: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

static int _3_update_spectrum_peak_hold() {
    int rc;
    // spectrum peak hold from 1ms to 1s
    rc = sqlite3_exec(db, "UPDATE params SET val = val / 1000 WHERE name = 'spectrum_peak_hold'", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot update spectrum peak hold: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

/* Dial frequency presets of one type, added to an older params.db (a new one has them from
 * sql/params.sql: OR IGNORE leaves those rows alone). rows: "('label', Hz), ..." */
static int add_dial_presets(cfg_digital_type_t type, const char *rows, const char *what) {
    char *sql = NULL;
    int   err = 1;

    if (asprintf(&sql, "INSERT OR IGNORE INTO digital_modes(label, freq, mode, type) "
                       "SELECT column1, column2, 3, %u FROM (VALUES %s)", type, rows) < 0) {
        puts("Cannot allocate SQL query");
    } else if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        printf("Cannot add %s presets: %s\n", what, sqlite3_errmsg(db));
        free(sql);
    } else {
        err = 0;
        free(sql);
    }
    return err;
}

/* JS8Call's default dial frequencies (FrequencyList.cpp), USB-D */
static int _4_js8_dial_freqs() {
    return add_dial_presets(CFG_DIG_TYPE_JS8,
        "('JS8 160m', 1842000), ('JS8 80m', 3578000), ('JS8 40m', 7078000), ('JS8 30m', 10130000), "
        "('JS8 20m', 14078000), ('JS8 17m', 18104000), ('JS8 15m', 21078000), ('JS8 12m', 24922000), "
        "('JS8 10m', 28078000), ('JS8 6m', 50318000)", "JS8");
}

/* The JS8 nets of GhostNet 1.5 (s2underground): 7.107 MHz first, then 14.107 and 3.575 */
static int _5_ghostnet_dial_freqs() {
    return add_dial_presets(CFG_DIG_TYPE_JS8_GHOSTNET,
        "('GhostNet 80m', 3575000), ('GhostNet 40m', 7107000), ('GhostNet 20m', 14107000)", "GhostNet");
}

/* WSPR dial frequencies (WSJT-X's defaults) */
static int _6_add_wspr_presets() {
    int rc;
    char *query;
    rc = asprintf(&query,
        "INSERT OR IGNORE INTO digital_modes(label, freq, mode, type) "
        "SELECT column1, column2, 3, %u FROM (VALUES "
            "('WSPR 160m', 1836600),"
            "('WSPR 80m', 3568600),"
            "('WSPR 60m', 5287200),"
            "('WSPR 40m', 7038600),"
            "('WSPR 30m', 10138700),"
            "('WSPR 20m', 14095600),"
            "('WSPR 17m', 18104600),"
            "('WSPR 15m', 21094600),"
            "('WSPR 12m', 24924600),"
            "('WSPR 10m', 28124600),"
            "('WSPR 6m', 50293000)"
        ")",
        CFG_DIG_TYPE_WSPR
    );
    if (rc == -1) {
        printf("Cannot allocate SQL query\n");
        return 1;
    }
    rc = sqlite3_exec(db, query, NULL, NULL, NULL);
    free(query);
    if (rc != SQLITE_OK) {
        printf("Cannot add WSPR presets: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

/* FT2 dial frequencies (IU8LMC's Decodium list, from SP2KMO's upstream 76d98ce2) */
static int _7_add_ft2_presets() {
    int rc;
    char *query;
    rc = asprintf(&query,
        "INSERT OR IGNORE INTO digital_modes(label, freq, mode, type) "
        "SELECT column1, column2, 3, %u FROM (VALUES "
            "('FT2 160m', 1843000),"
            "('FT2 80m', 3568000),"
            "('FT2 60m', 5360000),"
            "('FT2 40m', 7062000),"
            "('FT2 30m', 10144000),"
            "('FT2 20m', 14084000),"
            "('FT2 17m', 18108000),"
            "('FT2 15m', 21144000),"
            "('FT2 12m', 24923000),"
            "('FT2 10m', 28184000),"
            "('FT2 6m', 50316000)"
        ")",
        CFG_DIG_TYPE_FT2
    );
    if (rc == -1) {
        printf("Cannot allocate SQL query\n");
        return 1;
    }
    rc = sqlite3_exec(db, query, NULL, NULL, NULL);
    free(query);
    if (rc != SQLITE_OK) {
        printf("Cannot add FT2 presets: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

/* Compressor threshold and makeup offsets, 0.100 to 0.126: Settings saved the dB value
 * itself instead of twice it, read back halved after a restart. Only a half-dB value is
 * unambiguous (stored as a real; correct rows are always integers): 3.5 becomes 7. */
static int _8_fix_comp_offsets() {
    if (sqlite3_exec(db, "UPDATE params SET val = CAST(round(val * 2) AS INTEGER) "
                         "WHERE name IN ('comp_threshold_offset', 'comp_makeup_offset') AND typeof(val) = 'real'",
                     NULL, NULL, NULL) != SQLITE_OK) {
        printf("Cannot fix compressor offsets: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    return 0;
}

static int (*migrations[])() = {
    _0_init_migrations,
    _1_create_ftx_table,
    _2_update_atu_freq,
    _3_update_spectrum_peak_hold,
    _4_js8_dial_freqs,
    _5_ghostnet_dial_freqs,
    _6_add_wspr_presets,
    _7_add_ft2_presets,
    _8_fix_comp_offsets,
};

int migrations_apply(void) {
    int rc = 0;
    int ver;
    if (db == NULL) {
        printf("Database is not opened\n");
        return 1;
    }

    rc = get_current_version(&ver);
    if (rc != 0) {
        printf("Cannot get current version\n");
        return 1;
    }
    for (size_t i = ver+1; i < SIZEOF_ARRAY(migrations); i++){
        printf("Apply migration: %i ...\n", i);
        rc = (*migrations[i])();
        if (rc != 0) {
            printf("Can't apply %i migration\n", i);
            break;
        }
        set_current_version(i);
    }
    return rc;
}


static int get_current_version(int * ver) {
    int rc;
    sqlite3_stmt *stmt;
    rc = sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS version(id INT NOT NULL DEFAULT 0)", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("Cannot create versions table: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    rc = sqlite3_prepare_v2(db, "SELECT id from version", -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        printf("Failed prepare statement: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
        *ver = sqlite3_column_int(stmt, 0);
    } else {
        *ver = -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int set_current_version(int ver) {
    int rc;
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "UPDATE version SET id=?", -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        printf("Failed prepare statement: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_bind_int(stmt, 1, ver);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return 0;
}

/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the Hand mic UP/DOWN setting (#300): the real settings on a database made from sql/params.sql, each
 *  start of the radio a SettingsManager of its own on it, the rows read back after the pending writes are flushed.
 *  The cases:
 *  - a radio that never chose (new, or updated from a version without it: no row): Channels, and a start writes no
 *    row, so the keys do what they did;
 *  - Tune chosen in Settings: its row, and the next start has it; Channels chosen again: its row, the next start too;
 *  - a value the setting cannot hold (a row written by hand): within Channels and Tune.
 *    mic_updown_param_test <params.sql>
 */

#include "cfg/settings_manager.h"

extern "C" {
#include "mic_updown.h"
}

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#define NONE (-999)

static sqlite3 *db;
static int      fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

static void exec(const std::string &q) {
    char *err = nullptr;

    if (sqlite3_exec(db, q.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        printf("SQL failed: %s\n", err ? err : "?");
        exit(2);
    }
}

/* The stored value, NONE without a row; the pending writes flushed first, as the flush thread does */
static int row(SettingsManager &sm) {
    sqlite3_stmt *st;
    int           v = NONE;

    sm.flush_all();
    sqlite3_prepare_v2(db, "SELECT val FROM params WHERE name = 'mic_updown'", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* A start of the radio on the database as it is */
static std::unique_ptr<SettingsManager> start() {
    auto sm = std::make_unique<SettingsManager>();

    sm->init_load(nullptr);
    return sm;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: mic_updown_param_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;

    sql << f.rdbuf();
    check(f.good() && sqlite3_open(":memory:", &db) == SQLITE_OK, "params.sql and the database opened", 0);
    exec(sql.str());
    check(cfg_db_init(db), "cfg_db_init", 0);

    /* Never chosen: a new database has no row, nor has a radio updated from a version without the setting */
    {
        auto sm = start();
        check(sm->p_mic_updown.get() == MIC_UPDOWN_CHANNELS, "never chosen: Channels", sm->p_mic_updown.get());
        check(row(*sm) == NONE, "never chosen: no row from a start", row(*sm));
        sm->p_kbd_layout.set(1);
        check(row(*sm) == NONE, "another Keys setting changed: still no row", row(*sm));
    }

    /* Tune chosen (Settings' dropdown sets the index), then Channels again */
    {
        auto sm = start();
        sm->p_mic_updown.set(MIC_UPDOWN_TUNE);
        check(row(*sm) == MIC_UPDOWN_TUNE, "Tune chosen: its row", row(*sm));
    }
    {
        auto sm = start();
        check(sm->p_mic_updown.get() == MIC_UPDOWN_TUNE, "Tune after the next start", sm->p_mic_updown.get());
        sm->p_mic_updown.set(MIC_UPDOWN_CHANNELS);
        check(row(*sm) == MIC_UPDOWN_CHANNELS, "Channels chosen again: its row", row(*sm));
    }
    {
        auto sm = start();
        check(sm->p_mic_updown.get() == MIC_UPDOWN_CHANNELS, "Channels after the next start", sm->p_mic_updown.get());
    }

    /* Rows the dropdown never writes */
    exec("UPDATE params SET val = 7 WHERE name = 'mic_updown'");
    {
        auto sm = start();
        check(sm->p_mic_updown.get() == MIC_UPDOWN_TUNE, "a row of 7: Tune, the last choice", sm->p_mic_updown.get());
    }
    exec("UPDATE params SET val = -3 WHERE name = 'mic_updown'");
    {
        auto sm = start();
        check(sm->p_mic_updown.get() == MIC_UPDOWN_CHANNELS, "a row of -3: Channels", sm->p_mic_updown.get());
    }

    printf("mic_updown_param: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

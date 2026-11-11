/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of KEY Iambic's default (#109). The real settings on a database made from sql/params.sql, each start of
 *  the radio a SettingsManager of its own on it (init_load, as params_init does before radio.c sends the KEY settings
 *  to the base), the rows read back after the pending writes are flushed. The cases:
 *  - a radio that never changed Iambic, new or updated (no row): B, and a start writes no row;
 *  - a radio set to A, or to B, on an earlier version (its row): keeps it;
 *  - Iambic changed as the KEY button does: the row, and the next start has it, A too.
 *    iambic_default_x6100 / iambic_default_x6200 <params.sql>
 */

#include "cfg/settings_manager.h"

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
    sqlite3_prepare_v2(db, "SELECT val FROM params WHERE name = 'iambic_mode'", -1, &st, nullptr);
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

/* The KEY button (controls_toggle_key_iambic_mode) */
static void toggle(SettingsManager &sm) {
    sm.p_iambic_mode.set(sm.p_iambic_mode.get() == radio_iambic_a ? radio_iambic_b : radio_iambic_a);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: iambic_default_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;

    sql << f.rdbuf();
    check(f.good() && sqlite3_open(":memory:", &db) == SQLITE_OK, "params.sql and the database opened", 0);
    exec(sql.str());
    check(cfg_db_init(db), "cfg_db_init", 0);

    /* Never changed: a new database has no row, nor has a radio updated without a change */
    {
        auto sm = start();
        check(sm->p_iambic_mode.get() == radio_iambic_b, "never changed: B", sm->p_iambic_mode.get());
        check(row(*sm) == NONE, "never changed: no row from a start", row(*sm));
        sm->p_key_speed.set(25);
        check(row(*sm) == NONE, "another KEY setting changed: still no row", row(*sm));
    }

    /* Set to A on an earlier version, where A was the start: B and back, the row 0 */
    exec("INSERT INTO params VALUES ('iambic_mode', 0)");
    {
        auto sm = start();
        check(sm->p_iambic_mode.get() == radio_iambic_a, "set to A: keeps A", sm->p_iambic_mode.get());
        check(row(*sm) == radio_iambic_a, "set to A: its row as it was", row(*sm));
    }

    /* Set to B on an earlier version */
    exec("UPDATE params SET val = 1 WHERE name = 'iambic_mode'");
    {
        auto sm = start();
        check(sm->p_iambic_mode.get() == radio_iambic_b, "set to B: keeps B", sm->p_iambic_mode.get());
    }

    /* Changed on this version, from the start B: A stays A at the next start, and B again stays B */
    exec("DELETE FROM params WHERE name = 'iambic_mode'");
    {
        auto sm = start();
        toggle(*sm);
        check(sm->p_iambic_mode.get() == radio_iambic_a, "the KEY button from B: A", sm->p_iambic_mode.get());
        check(row(*sm) == radio_iambic_a, "the KEY button to A: its row", row(*sm));
    }
    {
        auto sm = start();
        check(sm->p_iambic_mode.get() == radio_iambic_a, "A after the next start", sm->p_iambic_mode.get());
        toggle(*sm);
        check(row(*sm) == radio_iambic_b, "the KEY button back to B: its row", row(*sm));
    }
    {
        auto sm = start();
        check(sm->p_iambic_mode.get() == radio_iambic_b, "B after the next start", sm->p_iambic_mode.get());
    }

    printf("iambic_default (%s): %d checks, %d failed\n",
#ifdef BOARD_X6200
           "X6200",
#else
           "X6100",
#endif
           checks, fails);
    return fails != 0;
}

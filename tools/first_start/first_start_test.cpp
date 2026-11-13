/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of when the first-start guide comes up by itself (src/first_start_rule.h), and of its ESC. The rule on
 *  its own, then on the real settings: a params.db made from sql/params.sql in a folder of its own, each start of the
 *  radio a SettingsManager of its own on it (init_load, as params_init does), the callsign read from its row as
 *  params.c reads it, the rows read back after the pending writes are flushed. The cases:
 *  - no callsign (none, empty, spaces) and no mark: the guide; a callsign, the mark, or a start on the defaults
 *    that saves nothing: none;
 *  - a new card: the guide, and a start writes no mark; the guide closed: the mark's row, and no guide at the next
 *    start, even with still no callsign;
 *  - a radio updated from before the guide, with its callsign and no mark: no guide, and no mark written;
 *  - Settings > Info > Factory reset (factory_reset.h deletes params.db): the guide at the next start;
 *  - ESC: the first asks, a second within FIRST_START_ESC_MS skips, a later one asks again, and so across the tick
 *    going round.
 *    first_start_x6100 / first_start_x6200 <params.sql>
 */

#include "cfg/settings_manager.h"
#include "cfg/parameter_api.h"
#include "factory_reset.h"

extern "C" {
#include "first_start_rule.h"
}

#include <sqlite3.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#define NONE (-999)

static sqlite3     *db;
static std::string  sql, dir, path;
static int          fails, checks;

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

/* params.db as a new card or a factory reset leaves it: the defaults (the GUI copies its default database in) */
static void card_new(void) {
    check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "params.db opened", 0);
    exec(sql);
    check(cfg_db_init(db), "cfg_db_init", 0);
}

static void card_close(void) {
    cfg_db_shutdown();
    sqlite3_close(db);
    db = nullptr;
}

/* The mark's row, NONE without one; the pending writes flushed first, as the flush thread does */
static int mark_row(SettingsManager &sm) {
    sqlite3_stmt *st;
    int           v = NONE;

    sm.flush_all();
    sqlite3_prepare_v2(db, "SELECT val FROM params WHERE name = 'first_start_done'", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* A start of the radio on the database as it is, and whether the guide comes up (first_start.c's due()) */
struct Start {
    std::unique_ptr<SettingsManager> sm;
    std::string                      callsign;
    bool                             due;
};

static Start start(void) {
    Start         s;
    sqlite3_stmt *st;

    s.sm = std::make_unique<SettingsManager>();
    s.sm->init_load(nullptr);
    sqlite3_prepare_v2(db, "SELECT val FROM params WHERE name = 'callsign'", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) {
        s.callsign = (const char *)sqlite3_column_text(st, 0);
    }
    sqlite3_finalize(st);
    s.due = first_start_due(s.callsign.c_str(), param_i_get(&s.sm->p_first_start_done), false);
    return s;
}

static void rule(void) {
    check(first_start_due(nullptr, false, false), "no callsign at all: the guide", 0);
    check(first_start_due("", false, false), "empty callsign: the guide", 0);
    check(first_start_due("   ", false, false), "callsign of spaces: the guide", 0);
    check(first_start_due(" \t", false, false), "callsign of a space and a tab: the guide", 0);
    check(!first_start_due("ON6URE", false, false), "a callsign: none", 0);
    check(!first_start_due(" ON6URE ", false, false), "a callsign between spaces: none", 0);
    check(!first_start_due("", true, false), "the mark: none", 0);
    check(!first_start_due("ON6URE", true, false), "a callsign and the mark: none", 0);
    check(!first_start_due("", false, true), "a start on the defaults, nothing saved: none", 0);
}

static void esc(void) {
    first_start_esc_t e = {};

    check(!first_start_esc(&e, 1000), "the first ESC asks", 0);
    check(first_start_esc(&e, 2000), "a second within the time skips", 0);
    check(!first_start_esc(&e, 3000), "the next asks again", 0);

    first_start_esc_reset(&e);
    check(!first_start_esc(&e, 10000), "after a reset: asks", 0);
    check(first_start_esc(&e, 10000 + FIRST_START_ESC_MS), "right at the time: skips", 0);

    first_start_esc_reset(&e);
    check(!first_start_esc(&e, 20000), "asks", 0);
    check(!first_start_esc(&e, 20000 + FIRST_START_ESC_MS + 1), "past the time: asks again", 0);
    check(first_start_esc(&e, 20000 + FIRST_START_ESC_MS + 500), "and the one after that skips", 0);

    first_start_esc_reset(&e);
    check(!first_start_esc(&e, 20000), "asks", 0);
    first_start_esc_reset(&e);      /* something else done in the guide */
    check(!first_start_esc(&e, 20500), "something done in between: asks again", 0);

    first_start_esc_reset(&e);
    check(!first_start_esc(&e, 0xFFFFFC18u), "asks 1 s before the tick goes round", 0);
    check(first_start_esc(&e, 1000), "skips 1 s after it", 0);
    first_start_esc_reset(&e);
    check(!first_start_esc(&e, 0xFFFFF000u), "asks 4.1 s before the tick goes round", 0);
    check(!first_start_esc(&e, 1500), "past the time across it: asks again", 0);
}

int main(int argc, char **argv) {
    char tmpl[] = "/tmp/first_start_test-XXXXXX";

    if (argc != 2) {
        fprintf(stderr, "usage: first_start_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream ss;

    ss << f.rdbuf();
    sql = ss.str();
    check(f.good() && mkdtemp(tmpl), "params.sql read, a folder made", 0);
    dir  = tmpl;
    path = dir + "/params.db";

    rule();
    esc();

    /* A new card: the guide, and a start writes no mark */
    card_new();
    {
        Start s = start();
        check(s.due, "new card: the guide", 0);
        check(mark_row(*s.sm) == NONE, "new card: a start writes no mark", mark_row(*s.sm));
        s.sm->p_volume.set(20);
        check(mark_row(*s.sm) == NONE, "another setting changed: still no mark", mark_row(*s.sm));
        param_i_set(&s.sm->p_first_start_done, 1);      /* the guide closed: first_start.c's mark_done() */
        check(mark_row(*s.sm) == 1, "the guide closed: the mark's row", mark_row(*s.sm));
    }
    {
        Start s = start();
        check(param_i_get(&s.sm->p_first_start_done) == 1, "next start: the mark", 0);
        check(!s.due, "next start, still no callsign: no guide", 0);
    }

    /* A callsign set in the guide: the same, and clearing it later does not bring the guide back */
    exec("INSERT INTO params VALUES ('callsign', 'ON6URE')");
    check(!start().due, "the mark and a callsign: no guide", 0);
    exec("UPDATE params SET val = '' WHERE name = 'callsign'");
    check(!start().due, "the callsign cleared later: no guide", 0);

    /* A radio updated from before the guide: its callsign, no mark */
    exec("DELETE FROM params WHERE name = 'first_start_done'");
    exec("UPDATE params SET val = 'ON6URE' WHERE name = 'callsign'");
    {
        Start s = start();
        check(s.callsign == "ON6URE", "updated radio: its callsign read", 0);
        check(!s.due, "updated radio with a callsign: no guide", 0);
        check(mark_row(*s.sm) == NONE, "updated radio: no mark written", mark_row(*s.sm));
    }

    /* Settings > Info > Factory reset, the mark and the callsign set: the guide at the next start */
    exec("INSERT INTO params VALUES ('first_start_done', 1)");
    check(!start().due, "before the reset: no guide", 0);
    card_close();
    {
        struct stat sb;
        int         n = factory_reset_dir(dir.c_str());

        check(n >= 1, "factory reset: files deleted", n);
        check(stat(path.c_str(), &sb) != 0, "factory reset: params.db gone", 0);
    }
    card_new();
    {
        Start s = start();
        check(s.callsign.empty(), "after the reset: no callsign", 0);
        check(s.due, "after the reset: the guide", 0);
    }
    card_close();
    factory_reset_dir(dir.c_str());
    rmdir(dir.c_str());

    printf("first_start (%s): %d checks, %d failed\n",
#ifdef BOARD_X6200
           "X6200",
#else
           "X6100",
#endif
           checks, fails);
    return fails != 0;
}

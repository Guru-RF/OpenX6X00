/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of Settings > Display > Tuning steps in the real settings (src/cfg), on a database made from
 *  sql/params.sql. FST's step change (cfg_mode_change_freq_step) and the knob's stop (cfg_freq_step_at_end) with the
 *  range at its default (the whole list, as before) and narrowed to 10-500 Hz; the range kept at the next start.
 *    freq_steps_cfg_x6100 / freq_steps_cfg_x6200 <params.sql>
 */

#include "cfg/settings_manager.h"
#include "cfg/settings_manager_api.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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

/* FST pressed: the new step */
static long fst(bool up = true) {
    return cfg_mode_change_freq_step(up);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: freq_steps_cfg_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;

    sql << f.rdbuf();
    check(f.good() && sqlite3_open(":memory:", &db) == SQLITE_OK, "params.sql and the database opened", 0);
    exec(sql.str());
    check(cfg_db_init(db), "cfg_db_init", 0);
    cfg_sm.init_load(nullptr);

    /* The default: the whole list, round at both ends, as FST always went */
    check(cfg_sm.p_freq_step_lo.get() == 0 && cfg_sm.p_freq_step_hi.get() == 7, "default: 10 Hz to 100 kHz",
          cfg_sm.p_freq_step_hi.get());
    cfg_sm.p_mode_freq_step.set(10);
    long want[] = {100, 500, 1000, 5000, 10000, 25000, 100000, 10};
    for (long w : want) {
        long got = fst();
        check(got == w, "default: FST up", got);
    }
    check(fst(false) == 100000, "default: FST down from 10 Hz goes round to 100 kHz", cfg_sm.p_mode_freq_step.get());
    check(cfg_freq_step_at_end(true), "default: a knob stops at 100 kHz", 0);
    cfg_sm.p_mode_freq_step.set(10);
    check(cfg_freq_step_at_end(false), "default: a knob stops at 10 Hz", 0);
    check(!cfg_freq_step_at_end(true), "default: a knob goes up from 10 Hz", 0);

    /* 10 Hz to 500 Hz */
    cfg_sm.p_freq_step_lo.set(0);
    cfg_sm.p_freq_step_hi.set(2);
    check(fst() == 100 && fst() == 500 && fst() == 10, "10-500 Hz: FST 10, 100, 500 and round to 10",
          cfg_sm.p_mode_freq_step.get());
    check(fst(false) == 500, "10-500 Hz: FST down from 10 Hz goes round to 500 Hz", cfg_sm.p_mode_freq_step.get());
    check(cfg_freq_step_at_end(true), "10-500 Hz: a knob stops at 500 Hz", 0);
    cfg_sm.p_mode_freq_step.set(100000);   /* chosen before the range was narrowed */
    check(cfg_freq_step_at_end(true), "10-500 Hz: a knob at 100 kHz turns no higher", 0);
    check(fst(false) == 500, "10-500 Hz: from 100 kHz down into the range", cfg_sm.p_mode_freq_step.get());

    /* The ends as the web configurator or a hand-edited database could give them: held to the list's 8 steps, and
     * in either order (Settings shows them as chosen) */
    cfg_sm.p_freq_step_lo.set(-3);
    cfg_sm.p_freq_step_hi.set(99);
    check(cfg_sm.p_freq_step_lo.get() == 0 && cfg_sm.p_freq_step_hi.get() == FREQ_STEPS_N - 1,
          "the ends held to 0 and the list's last", cfg_sm.p_freq_step_hi.get());
    cfg_sm.p_freq_step_lo.set(5);   /* 10 kHz to 500 Hz: 500 Hz to 10 kHz */
    cfg_sm.p_freq_step_hi.set(2);
    cfg_sm.p_mode_freq_step.set(10000);
    check(fst() == 500, "10 kHz to 500 Hz: FST round from 10 kHz to 500 Hz", cfg_sm.p_mode_freq_step.get());
    check(cfg_freq_step_at_end(false), "10 kHz to 500 Hz: a knob stops at 500 Hz", 0);
    cfg_sm.p_freq_step_lo.set(0);
    cfg_sm.p_freq_step_hi.set(2);

    /* Kept at the next start */
    cfg_sm.flush_all();
    {
        SettingsManager sm;

        sm.init_load(nullptr);
        check(sm.p_freq_step_lo.get() == 0 && sm.p_freq_step_hi.get() == 2, "the next start: 10-500 Hz kept",
              sm.p_freq_step_hi.get());
    }

    printf("freq_steps_cfg (%s): %d checks, %d failed\n",
#ifdef BOARD_X6200
           "X6200",
#else
           "X6100",
#endif
           checks, fails);
    return fails != 0;
}

/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of WFM's width in the real settings (#283), on a database made from sql/params.sql, as the X6200's
 *  Filter BW knob, button and info bar read and set it (cp_cur_filter_bw) and its Filter high and the browser Panel
 *  the edge (cp_cur_filter_high). The cases:
 *  - WFM's default: the edge 8000, shown 16000 wide, the stock firmware's 16 kHz;
 *  - a width set: the edge half of it, up to 16000 and no further, the narrowest 20;
 *  - the edge set past 8000: 8000; an edge of 10000 saved by v0.3.12: 8000 at the next start;
 *  - AM, NFM and SAM as before: the width their edge, up to 6000.
 *    wfm_settings_test <params.sql>
 */

#include "cfg/settings_manager.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

static sqlite3 *db;
static int      fails, checks;

static void eq(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s: %ld, want %ld\n", what, got, want);
    }
}

static void exec(const std::string &q) {
    char *err = nullptr;

    if (sqlite3_exec(db, q.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        printf("SQL failed: %s\n", err ? err : "?");
        exit(2);
    }
}

/* A start of the radio on the database as it is, in a mode */
static std::unique_ptr<SettingsManager> start(radio_mode_t mode) {
    auto sm = std::make_unique<SettingsManager>();

    sm->init_load(nullptr);
    sm->cp_cur_mode.set(mode);
    return sm;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: wfm_settings_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;

    sql << f.rdbuf();
    if (!f.good() || sqlite3_open(":memory:", &db) != SQLITE_OK) {
        printf("params.sql or the database: not opened\n");
        return 2;
    }
    exec(sql.str());
    if (!cfg_db_init(db)) {
        printf("cfg_db_init failed\n");
        return 2;
    }

    {
        auto sm = start(radio_mode_wfm);

        eq("WFM's default edge", sm->cp_cur_filter_high.get(), 8000);
        eq("WFM's default width", sm->cp_cur_filter_bw.get(), 16000);
        eq("WFM's low edge", sm->cp_cur_filter_low.get(), 0);

        sm->cp_cur_filter_bw.set(12000);
        eq("12 kHz wide: the edge", sm->cp_cur_filter_high.get(), 6000);
        eq("12 kHz wide: the width", sm->cp_cur_filter_bw.get(), 12000);
        sm->cp_cur_filter_bw.set(12100);
        eq("a knob detent wider", sm->cp_cur_filter_bw.get(), 12100);
        sm->cp_cur_filter_bw.set(20000);
        eq("past the base's widest: the edge", sm->cp_cur_filter_high.get(), 8000);
        eq("past the base's widest: the width", sm->cp_cur_filter_bw.get(), 16000);
        sm->cp_cur_filter_bw.set(0);
        eq("the narrowest width", sm->cp_cur_filter_bw.get(), 20);

        sm->cp_cur_filter_high.set(10000);
        eq("Filter high past 8000: the edge", sm->cp_cur_filter_high.get(), 8000);
        eq("Filter high past 8000: the width", sm->cp_cur_filter_bw.get(), 16000);
        sm->cp_cur_filter_high.set(5000);
        eq("Filter high 5000: the width", sm->cp_cur_filter_bw.get(), 10000);

        /* to NFM and back: each its own, the width of NFM its edge */
        sm->cp_cur_mode.set(radio_mode_nfm);
        eq("NFM's edge", sm->cp_cur_filter_high.get(), 4000);
        eq("NFM's width, the edge as before", sm->cp_cur_filter_bw.get(), 4000);
        sm->cp_cur_filter_bw.set(9000);
        eq("NFM no wider than 6000", sm->cp_cur_filter_bw.get(), 6000);
        sm->cp_cur_mode.set(radio_mode_wfm);
        eq("back in WFM: its own width", sm->cp_cur_filter_bw.get(), 10000);
        sm->flush_all();
    }

    /* An edge of 10000 saved by v0.3.12 (the base took it as 16 kHz): 8000 now, 16000 wide */
    exec("INSERT INTO mode_params VALUES (9, 'filter_high', 10000)");
    {
        auto sm = start(radio_mode_wfm);

        eq("v0.3.12's 10000: the edge", sm->cp_cur_filter_high.get(), 8000);
        eq("v0.3.12's 10000: the width", sm->cp_cur_filter_bw.get(), 16000);
    }

    /* AM and SAM as before */
    for (radio_mode_t m : {radio_mode_am, radio_mode_sam}) {
        auto sm = start(m);

        sm->cp_cur_filter_bw.set(5000);
        eq("AM/SAM: the width is the edge", sm->cp_cur_filter_high.get(), 5000);
        eq("AM/SAM: the width", sm->cp_cur_filter_bw.get(), 5000);
        sm->cp_cur_filter_bw.set(16000);
        eq("AM/SAM no wider than 6000", sm->cp_cur_filter_bw.get(), 6000);
    }

    printf("wfm_settings: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

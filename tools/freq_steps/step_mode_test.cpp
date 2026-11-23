/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the tuning step a mode comes back with, in the real settings (src/cfg) on a database made from
 *  sql/params.sql (#348). With Zoom and step per mode on, the main knob turned down from 14.100.000 USB in 1 kHz
 *  steps went 14.069.000, 13.900.000, 13.800.000: at 14.069.000 the band plan set CW, and CW came back with a
 *  100 kHz step that was not the one it was left with.
 *
 *  Where that step came from: Step per zoom keeps a step for each zoom (p_mode_step_z*), written only while the
 *  option is on. A mode's step chosen while it was off left the zoom's step as it was, and the two no longer agreed.
 *  With the option on again, a mode change that brings another zoom but the same step (SSB at 100 kHz with 1 kHz to
 *  CW at 25 kHz with 1 kHz) told step_zoom() of the zoom only, and it took the zoom's old step for the mode's. With
 *  two different steps the step was told first and the zoom's step followed it, so it showed only between modes on
 *  the same step.
 *
 *  The knob here is screen_main.c's freq_shift() and plan_mode() in small: a detent is the mode's step, the
 *  frequency aligned to it, and the mode from the band plan, CW under 14.070 MHz as Region 2 and the US plans have
 *  20 m, SSB above. The settings under it are the real ones.
 *    step_mode_x6100 / step_mode_x6200 <params.sql>
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

static bool cw_segment(int32_t f) {
    return f >= 14000000 && f < 14070000;
}

/* One detent of the main knob, turned slowly (no acceleration): the new frequency */
static long detent(int dir) {
    int32_t old  = cfg_sm.cp_fg_freq.get();
    int32_t df   = dir * cfg_sm.p_mode_freq_step.get();
    int32_t freq = old + df - (old + df) % abs(df);
    int32_t mode = cfg_sm.cp_cur_mode.get();

    cfg_sm.cp_fg_freq.set(freq);
    if (cw_segment(freq) && !cw_segment(old) && (mode == radio_mode_usb || mode == radio_mode_lsb)) {
        cfg_sm.cp_cur_mode.set(radio_mode_cw);
    } else if (freq >= 14070000 && cw_segment(old) && mode == radio_mode_cw) {
        cfg_sm.cp_cur_mode.set(radio_mode_usb);
    }
    return cfg_sm.cp_fg_freq.get();
}

static long step() {
    return cfg_sm.p_mode_freq_step.get();
}

/* FST until the step is want (it goes round) */
static void fst_to(int32_t want) {
    for (int i = 0; i < FREQ_STEPS_N && step() != want; i++) {
        cfg_mode_change_freq_step(true);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: step_mode_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;
    char             *err = nullptr;

    sql << f.rdbuf();
    check(f.good() && sqlite3_open(":memory:", &db) == SQLITE_OK, "params.sql and the database opened", 0);
    if (sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        printf("SQL failed: %s\n", err ? err : "?");
        return 2;
    }
    check(cfg_db_init(db), "cfg_db_init", 0);
    cfg_sm.init_load(nullptr);
    check(cfg_sm.p_zoom_step_per_mode.get() == 1, "Zoom and step per mode is on by default", 0);
    check(cfg_sm.p_step_per_zoom.get() == 0, "Step per zoom is off by default", 0);

    /* How the radio gets there. CW at its zoom, with Step per zoom on, once had 100 kHz: the zoom's step */
    cfg_sm.cp_fg_freq.set(14030000);
    cfg_sm.cp_cur_mode.set(radio_mode_cw);
    cfg_sm.p_mode_zoom.set(4);
    cfg_sm.p_step_per_zoom.set(1);
    fst_to(100000);
    check(step() == 100000, "CW: 100 kHz chosen with Step per zoom on", step());
    /* Step per zoom off, and CW's step put to 1 kHz: the zoom's step is not written */
    cfg_sm.p_step_per_zoom.set(0);
    fst_to(1000);
    check(step() == 1000, "CW: 1 kHz chosen with Step per zoom off", step());
    /* USB at 14.100.000 with 1 kHz, the zoom at 100 kHz, and Step per zoom on again */
    cfg_sm.cp_fg_freq.set(14100000);
    cfg_sm.cp_cur_mode.set(radio_mode_usb);
    cfg_sm.p_mode_zoom.set(1);
    fst_to(1000);
    cfg_sm.p_step_per_zoom.set(1);
    check(step() == 1000 && cfg_sm.cp_fg_freq.get() == 14100000, "USB: 1 kHz at 14.100.000", step());

    /* The report: down a detent at a time. 14.070.000 is the last in USB */
    long at = 0;

    for (int i = 0; i < 30; i++) {
        at = detent(-1);
    }
    check(at == 14070000 && cfg_sm.cp_cur_mode.get() == radio_mode_usb, "30 detents down: 14.070.000 USB", at);
    at = detent(-1);
    check(at == 14069000 && cfg_sm.cp_cur_mode.get() == radio_mode_cw, "the next: 14.069.000, CW from the band plan", at);
    check(step() == 1000, "CW comes back with the step it was left with, 1 kHz", step());
    check(cfg_sm.p_mode_zoom.get() == 4, "and with its zoom", cfg_sm.p_mode_zoom.get());
    at = detent(-1);
    check(at == 14068000, "a detent down in CW: 14.068.000 (it was 13.900.000)", at);
    at = detent(-1);
    check(at == 14067000, "and one more: 14.067.000 (in the report 13.800.000)", at);

    /* CW on 100 Hz from here: back up into SSB its 1 kHz again, and CW's 100 Hz the next time down */
    fst_to(100);
    for (int i = 0; i < 30; i++) {
        at = detent(+1);
    }
    check(at == 14070000 && cfg_sm.cp_cur_mode.get() == radio_mode_usb, "30 detents up: 14.070.000, USB again", at);
    check(step() == 1000, "USB comes back with 1 kHz", step());
    at = detent(+1);
    check(at == 14071000, "a detent up in USB: 14.071.000", at);
    detent(-1);
    at = detent(-1);
    check(at == 14069000 && step() == 100, "down into CW again: 14.069.000 and 100 Hz", step());

    /* Step per zoom still does its work in a mode: a zoom change brings the step chosen at that zoom back */
    cfg_sm.p_mode_zoom.set(1);
    fst_to(1000);
    cfg_sm.p_mode_zoom.set(4);
    check(step() == 100, "CW: back at the 25 kHz zoom, its 100 Hz", step());
    cfg_sm.p_mode_zoom.set(1);
    check(step() == 1000, "CW: back at the 100 kHz zoom, the 1 kHz chosen there", step());
    cfg_sm.p_mode_zoom.set(4);

    /* The same through Zoom and step per mode turned off and on: off, CW's step for every mode; on again in USB,
     * USB's own; and CW still comes back with its own */
    cfg_sm.p_zoom_step_per_mode.set(0);
    check(step() == 100, "every mode: the step in use stays", step());
    cfg_sm.cp_fg_freq.set(14100000);
    cfg_sm.cp_cur_mode.set(radio_mode_usb);
    check(step() == 100, "every mode: USB has it too", step());
    cfg_sm.p_zoom_step_per_mode.set(1);
    check(step() == 1000 && cfg_sm.p_mode_zoom.get() == 1, "per mode again: USB's own 1 kHz", step());
    cfg_sm.cp_cur_mode.set(radio_mode_cw);
    check(step() == 100 && cfg_sm.p_mode_zoom.get() == 4, "per mode again: CW's own 100 Hz", step());

    /* Kept at the next start */
    cfg_sm.flush_all();
    {
        SettingsManager sm;

        sm.init_load(nullptr);
        check(sm.p_mode_freq_step.get() == 100, "the next start in CW: 100 Hz", sm.p_mode_freq_step.get());
    }

    printf("step_mode (%s): %d checks, %d failed\n",
#ifdef BOARD_X6200
           "X6200",
#else
           "X6100",
#endif
           checks, fails);
    return fails != 0;
}
